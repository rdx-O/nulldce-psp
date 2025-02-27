#include "gdromv3.h"

#include "plugins/plugin_manager.h"
#include "dc/mem/sh4_mem.h"
#include "dc/mem/memutil.h"
#include "dc/mem/sb.h"
#include "dc/sh4/dmac.h"
#include "dc/sh4/intc.h"
#include "dc/sh4/sh4_registers.h"
#include "dc/asic/asic.h"
#include "../plugs/ImgReader/ImgReader.h"


#include "pspDmac.h"

enum gd_states
{
	//Generic
	gds_waitcmd,
	gds_procata,
	gds_waitpacket,
	gds_procpacket,
	gds_pio_send_data,
	gds_pio_get_data,
	gds_pio_end,
	gds_procpacketdone,

	//Command spec.
	gds_readsector_pio,
	gds_readsector_dma,
	gds_process_set_mode,
};

struct
{
	u32 start_sector;
	u32 remaining_sectors;
	u32 sector_type;
} read_params;

struct
{
	u32 index;
	union
	{
		u16 data_16[6]; //*WATCH* this is in native-endian form ..
		u8 data_8[12];
		//Spi command structs
		union
		{
			struct
			{
				u8 cc;

#if HOST_ENDIAN==ENDIAN_LITTLE
				u8 prmtype	: 1 ;
				u8 expdtype	: 3 ;
				//	u8 datasel	: 4 ;
				u8 other	: 1 ;
				u8 data		: 1 ;
				u8 subh		: 1 ;
				u8 head		: 1 ;
#else
				u8 head		: 1 ;
				u8 subh		: 1 ;
				u8 data		: 1 ;
				u8 other	: 1 ;
				u8 expdtype	: 3 ;
				u8 prmtype	: 1 ;
#endif

				u8 block[10];
			};

			struct
			{
				u8 b[12];
			};
		}GDReadBlock;
	};
} packet_cmd;

//Buffer for sector reads [ dma ]
struct
{
	u32 cache_index;
	u32 cache_size;
	u8 cache[2352*32];	//up to 32 sectors
} read_buff;

//pio buffer
struct
{
	gd_states next_state;
	u32 index;
	u32 size;
	u16 data[0x10000>>1];	//64 kb
} pio_buff;

u32 set_mode_offset;
struct
{
	u8 command;
} ata_cmd;

struct cdda_t
{
	enum { NoInfo, Playing, Paused, Terminated } status;
	u32 repeats;
	union
	{
		u32 FAD;
		struct
		{
			u8 B0; // MSB
			u8 B1; // Middle byte
			u8 B2; // LSB
		};
	}CurrAddr,EndAddr,StartAddr;
};

cdda_t cdda ;

gd_states gd_state;
DiscType gd_disk_type;
/*
	GD rom reset -> GDS_WAITCMD

	GDS_WAITCMD -> ATA/SPI command [Command code is on ata_cmd]
	SPI Command -> GDS_WAITPACKET -> GDS_SPI_* , depending on input

	GDS_SPI_READSECTOR -> Depending on features , it can do quite a few things
*/
u32 data_write_mode=0;

//Registers
	u32 DriveSel;
	GD_ErrRegT Error;
	GD_InterruptReasonT IntReason;
	GD_FeaturesT Features;
	GD_SecCountT SecCount;
	GD_SecNumbT SecNumber;

	GD_StatusT GDStatus;

	GD_HardwareInfo_t GD_HardwareInfo;

	union
	{
		struct
		{
		u8 low;
		u8 hi;
		};
		//u16 full;
		u16 Get() { return low | (hi<<8); }
		void Set(u16 v) { low=(u8)v; hi=v>>8; }
	} ByteCount;

//end

void nilprintf(...){}

#define printf_rm nilprintf
#define printf_ata nilprintf
#define printf_spi nilprintf
#define printf_spicmd nilprintf

u32 gd_get_subcode(u32 format, u32 fad, u8 *subc_info)
{
	subc_info[0] = 0;
	switch (cdda.status)
	{
	case cdda_t::NoInfo:
	default:
		subc_info[1] = 0x15;	// No audio status info
		break;
	case cdda_t::Playing:
		subc_info[1] = 0x11;	// Audio playback in progress
		break;
	case cdda_t::Paused:
		subc_info[1] = 0x12;	// Audio playback paused
		break;
	case cdda_t::Terminated:
		subc_info[1] = 0x13;	// Audio playback ended normally
		break;
	}

	switch (format)
	{
	case 0:	// Raw subcode
		subc_info[2] = 0;
		subc_info[3] = 100;
		libGDR_ReadSubChannel(subc_info + 4, 0, 100 - 4);
		break;

	case 1:	// Q data only
	default:
		{
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(fad, elapsed);

			//2 DATA Length MSB (0 = 0h)
			subc_info[2] = 0;
			//3 DATA Length LSB (14 = Eh)
			subc_info[3] = 0xE;
			//4 Control ADR
			subc_info[4] = (SecNumber.DiscFormat == 0 ? 0 : 0x40) | 1; // Control = 4 for data track
			//5-13	DATA-Q
			u8* data_q = &subc_info[5 - 1];
			//-When ADR = 1
			//1 TNO - track number
			data_q[1] = tracknum;
			//2 X - index within track
			data_q[2] = 1;
			//3-5   Elapsed FAD within track
			data_q[3] = elapsed >> 16;
			data_q[4] = elapsed >> 8;
			data_q[5] = elapsed;
			//6 ZERO
			data_q[6] = 0;
			//7-9 FAD
			data_q[7] = fad >> 16;
			data_q[8] = fad >> 8;
			data_q[9] = fad;
			/*DEBUG_LOG(GDROM, "gd_get_subcode: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
					 subc_info[0], subc_info[1], subc_info[2], subc_info[3],
					 subc_info[4], subc_info[5], subc_info[6], subc_info[7],
					 subc_info[8], subc_info[9], subc_info[10], subc_info[11],
					 subc_info[12], subc_info[13]);*/
		}
		break;

	case 2:	// Media catalog number (UPC/bar code)
		{
			//2 DATA Length MSB (0 = 0h)
			subc_info[2] = 0;
			//3 DATA Length LSB (24 = 18h)
			subc_info[3] = 0x18;
			//4 Format Code
			subc_info[4] = 2;
			//5-7 reserved
			subc_info[5] = 0;
			subc_info[6] = 0;
			subc_info[7] = 0;
			//8 MCVal (bit 7)
			subc_info[8] = 0;	// not valid
			//9-21 Media catalog number
			memcpy(&subc_info[9], "0000000000000", 13);
			//22-23 reserved
			subc_info[22] = 0;
			subc_info[23] = 0;
			//DEBUG_LOG(GDROM, "gd_get_subcode: format 2 (Media catalog number). audio %x", subc_info[1]);
		}
		break;
	}
	return subc_info[3];
}

void FASTCALL gdrom_get_cdda(s16* sector)
{
	//silence ! :p
	if (cdda.status == cdda_t::Playing)
	{
		libGDR_ReadSector((u8*)sector,cdda.CurrAddr.FAD,1,2352);
		cdda.CurrAddr.FAD++;
		if (cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
		{
			if (cdda.repeats==0)
			{
				//stop
				cdda.status = cdda_t::Terminated;
				SecNumber.Status = GD_PAUSE;
			}
			else
			{
				//Repeat ;)
				if (cdda.repeats!=0xf)
					cdda.repeats--;

				cdda.CurrAddr.FAD=cdda.StartAddr.FAD;
			}
		}
	}
	else
	{
		memset(sector,0,2352);
	}
}
void gd_spi_pio_end(u8* buffer,u32 len,gd_states next_state=gds_pio_end);
void gd_process_spi_cmd();
void gd_process_ata_cmd();
void FillReadBuffer()
{
	read_buff.cache_index=0;
	u32 count = read_params.remaining_sectors;
	if (count>32)
		count=32;

	read_buff.cache_size=count*read_params.sector_type;

	libGDR_ReadSector(read_buff.cache,read_params.start_sector,count,read_params.sector_type);
	read_params.start_sector+=count;
	read_params.remaining_sectors-=count;
}

void gd_set_state(gd_states state)
{
	gd_states prev=gd_state;
	gd_state=state;
	switch(state)
	{
		case gds_waitcmd:
			GDStatus.DRDY=1;	//Can accept ata cmd :)
			GDStatus.BSY=0;		//Does not access command block
			break;

		case gds_procata:
			//verify(prev==gds_waitcmd);	//validate the previus cmd ;)

			GDStatus.DRDY=0;	//can't accept ata cmd
			GDStatus.BSY=1;		//accessing command block to process cmd
			gd_process_ata_cmd();
			break;

		case gds_waitpacket:
			verify(prev==gds_procata);	//validate the previus cmd ;)

			//prepare for packet cmd
			packet_cmd.index=0;

			//Set CoD, clear BSY and IO
			IntReason.CoD=1;
			GDStatus.BSY = 0;
			IntReason.IO=0;

			//Make DRQ valid
			GDStatus.DRQ = 1;

			//ATA can optionaly raise the interrupt ...
			//RaiseInterrupt(holly_GDROM_CMD);
			break;

		case gds_procpacket:
			verify(prev==gds_waitpacket);	//validate the previus state ;)

			GDStatus.DRQ=0;		//can't accept ata cmd
			GDStatus.BSY=1;		//accessing command block to process cmd
			gd_process_spi_cmd();
			break;
			//yep , get/set are the same !
		case gds_pio_get_data:
		case gds_pio_send_data:
			//	When preparations are complete, the following steps are carried out at the device.
			//(1)	Number of bytes to be read is set in "Byte Count" register.
			ByteCount.Set((u16)(pio_buff.size<<1));
			//(2)	IO bit is set and CoD bit is cleared.
			IntReason.IO=1;
			IntReason.CoD=0;
			//(3)	DRQ bit is set, BSY bit is cleared.
			GDStatus.DRQ=1;
			GDStatus.BSY=0;
			//(4)	INTRQ is set, and a host interrupt is issued.
			asic_RaiseInterrupt(holly_GDROM_CMD);
			/*
			The number of bytes normally is the byte number in the register at the time of receiving
			the command, but it may also be the total of several devices handled by the buffer at that point.
			*/
			break;

		case gds_readsector_pio:
			{
				/*
				If more data are to be sent, the device sets the BSY bit and repeats the above sequence
				from step 7.
				*/
				GDStatus.BSY=1;

				u32 sector_count = read_params.remaining_sectors;
				gd_states next_state=gds_pio_end;

				if (sector_count>27)
				{
					sector_count=27;
					next_state=gds_readsector_pio;
				}
				libGDR_ReadSector((u8*)&pio_buff.data[0],read_params.start_sector,sector_count,
											read_params.sector_type);
				read_params.start_sector+=sector_count;
				read_params.remaining_sectors-=sector_count;

				gd_spi_pio_end(0,sector_count*read_params.sector_type,next_state);
			}
			break;

		case gds_readsector_dma:
			FillReadBuffer();
			break;

		case gds_pio_end:

			GDStatus.DRQ=0;//all data is sent !

			gd_set_state(gds_procpacketdone);
			break;

		case gds_procpacketdone:
			/*
			7.	When the device is ready to send the status, it writes the
			final status (IO, CoD, DRDY set, BSY, DRQ cleared) to the "Status" register before making INTRQ valid.
			After checking INTRQ, the host reads the "Status" register to check the completion status.
			*/
			//Set IO, CoD, DRDY
			GDStatus.DRDY=1;
			IntReason.CoD=1;
			IntReason.IO=1;

			//Clear DRQ,BSY
			GDStatus.DRQ=0;
			GDStatus.BSY=0;
			//Make INTRQ valid
			asic_RaiseInterrupt(holly_GDROM_CMD);

			//command finished !
			gd_set_state(gds_waitcmd);
			break;

		case gds_process_set_mode:
			sceKernelDcacheWritebackInvalidateAll();
			sceDmacMemcpy((u8 *)&GD_HardwareInfo + set_mode_offset, pio_buff.data, pio_buff.size << 1);
			//end pio transfer ;)
			gd_set_state(gds_pio_end);
			break;
		default :
			die("Unhandled gdrom state ...");
			break;
	}
}


void gd_setdisc()
{
	gd_disk_type = (DiscType)libGDR_GetDiscType();

	switch(gd_disk_type)
	{
	case NoDisk:
		SecNumber.Status = GD_NODISC;
		//GDStatus.BSY=0;
		//GDStatus.DRDY=1;
		break;
	case Open:
		SecNumber.Status = GD_OPEN;
		//GDStatus.BSY=0;
		//GDStatus.DRDY=1;
		break;
	case Busy:
		SecNumber.Status = GD_BUSY;
		//GDStatus.BSY=1;
		//GDStatus.DRDY=0;
		break;
	default :
		if (SecNumber.Status==GD_BUSY)
			SecNumber.Status = GD_PAUSE;
		else
			SecNumber.Status = GD_STANDBY;
		//GDStatus.BSY=0;
		//GDStatus.DRDY=1;
		break;
	}

	SecNumber.DiscFormat=gd_disk_type>>4;
}
void gd_reset()
{
	//Reset the drive
	gd_setdisc();
	gd_set_state(gds_waitcmd);
}
u32 GetFAD(u8* data,bool msf)
{
	if( msf )
	{
		printf("GDROM: MSF FORMAT\n");
		return ((data[0]*60*75) + (data[1]*75) + (data[2]));
	}
	else
	{
		return (data[0]<<16) | (data[1]<<8) | (data[2]);
	}
}
//disk changes ect
void NotifyEvent_gdrom(u32 info,void* param)
{
	if (info == DiskChange)
		gd_setdisc();
}

//This handles the work of setting up the pio regs/state :)
void gd_spi_pio_end(u8* buffer,u32 len,gd_states next_state)
{
	verify(len<0xFFFF);
	pio_buff.index=0;
	pio_buff.size=len>>1;
	pio_buff.next_state=next_state;
	if (buffer!=0){
		//memcpy(pio_buff.data,buffer,len);
		sceKernelDcacheWritebackInvalidateAll();
		sceDmacMemcpy(pio_buff.data, buffer, len);
	}
	if (len==0)
		gd_set_state(next_state);
	else
		gd_set_state(gds_pio_send_data);
}
void gd_spi_pio_read_end(u32 len,gd_states next_state)
{
	verify(len<0xFFFF);
	pio_buff.index=0;
	pio_buff.size=len>>1;
	pio_buff.next_state=next_state;
	if (len==0)
		gd_set_state(next_state);
	else
		gd_set_state(gds_pio_get_data);
}
void gd_process_ata_cmd()
{
	//Any ata cmd clears these bits , unless aborted/error :p
	Error.ABRT=0;
	GDStatus.CHECK=0;

	switch(ata_cmd.command)
	{
	case ATA_NOP:
		printf_ata("ATA_NOP\n");
		/*
			Setting "abort" in the error register
			Setting an error in the status register
			Clearing "busy" in the status register
			Asserting the INTRQ signal
		*/

		Error.ABRT=1;
		//Error.Sense=0x00; //fixme ?
		GDStatus.BSY=0;
		GDStatus.CHECK=1;

		asic_RaiseInterrupt(holly_GDROM_CMD);
		gd_set_state(gds_waitcmd);
		break;

	case ATA_SOFT_RESET:
		{
			printf_ata("ATA_SOFT_RESET\n");
			//DRV -> preserved -> wtf is it anyway ?
			gd_reset();
		}
		break;

	case ATA_EXEC_DIAG:
		printf_ata("ATA_EXEC_DIAG\n");
		printf("ATA_EXEC_DIAG -- not implemented\n");
		break;

	case ATA_SPI_PACKET:
		printf_ata("ATA_SPI_PACKET\n");
		gd_set_state(gds_waitpacket);
		break;

	case ATA_IDENTIFY_DEV:
		printf_ata("ATA_IDENTIFY_DEV\n");
		GDStatus.BSY = 0;
		gd_spi_pio_end((u8*)&gd_data_0x11[0], 0x50);
		break;

	case ATA_SET_FEATURES:
		printf_ata("ATA_SET_FEATURES\n");

		//Set features sets :
		//Error : ABRT
		Error.ABRT=0;	//command was not aborted ;) [hopefully ...]

		//status : DRDY , DSC , DF , CHECK
		//DRDY is set on state change
		GDStatus.DSC=0;
		GDStatus.DF=0;
		GDStatus.CHECK=0;
		asic_RaiseInterrupt(holly_GDROM_CMD);  //???
		gd_set_state(gds_waitcmd);
		break;

	default:
		die("Unkown ATA command..");
		break;
	};
}

void gd_process_spi_cmd()
{
	printf_spi("SPI cmd %02x;",packet_cmd.data_8[0]);
	printf_spi("params: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x \n",
		packet_cmd.data_8[0], packet_cmd.data_8[1], packet_cmd.data_8[2], packet_cmd.data_8[3], packet_cmd.data_8[4], packet_cmd.data_8[5],
		packet_cmd.data_8[6], packet_cmd.data_8[7], packet_cmd.data_8[8], packet_cmd.data_8[9], packet_cmd.data_8[10], packet_cmd.data_8[11] );

	GDStatus.CHECK=0;

	switch(packet_cmd.data_8[0])
	{
	case SPI_TEST_UNIT	:
		printf_spicmd("SPI_TEST_UNIT\n");

		GDStatus.CHECK=SecNumber.Status==GD_BUSY;	//drive is ready ;)

		gd_set_state(gds_procpacketdone);
		break;

	case SPI_REQ_MODE:
		GD_HardwareInfo.speed = 0;	// doesn't seem to be settable, or perhaps not for GD-Roms
		GD_HardwareInfo._res0[0] = 0;
		GD_HardwareInfo._res0[1] = 0;
		GD_HardwareInfo._res1 = 0;
		GD_HardwareInfo._res2[0] = 0;
		GD_HardwareInfo._res2[1] = 0;
		GD_HardwareInfo.read_flags &= 0x39;
		printf_spicmd("SPI_REQ_MODE\n");
		gd_spi_pio_end((u8*)&GD_HardwareInfo + packet_cmd.data_8[2], packet_cmd.data_8[4]);
		break;

		/////////////////////////////////////////////////
		// *FIXME* CHECK FOR DMA, Diff Settings !?!@$#!@%
	case SPI_CD_READ:
	case SPI_CD_READ2:
		{
#define readcmd packet_cmd.GDReadBlock

			u32 sector_type=2048;
			if (readcmd.head ==1 && readcmd.subh==1 && readcmd.data==1 && readcmd.expdtype==3 && readcmd.other==0)
				sector_type=2340;
			else if( readcmd.head ||readcmd.subh || readcmd.other || (!readcmd.data) )	// assert
				printf("GDROM: *FIXME* ADD MORE CD READ SETTINGS\n");

			read_params.start_sector = GetFAD(&packet_cmd.data_8[2],packet_cmd.GDReadBlock.prmtype);
			// *FIXME LIBRETRO* : packet_cmd.data_8 should probably be rebased to readcmd.b
			if (packet_cmd.data_8[0] == SPI_CD_READ)
				read_params.remaining_sectors = (packet_cmd.data_8[8]<<16) | (packet_cmd.data_8[9]<<8) | (packet_cmd.data_8[10]);
			else
				read_params.remaining_sectors = (packet_cmd.data_8[6] << 8) | packet_cmd.data_8[7];
			read_params.sector_type = sector_type;//yeah i know , not really many types supported...

			printf_spicmd("SPI_CD_READ sec=%d sz=%d/%d dma=%d\n",read_params.start_sector,read_params.remaining_sectors,read_params.sector_type,Features.CDRead.DMA);
			if (Features.CDRead.DMA==1)
			{
				gd_set_state(gds_readsector_dma);
			}
			else
			{
				gd_set_state(gds_readsector_pio);
			}
		}
		break;

	case SPI_GET_TOC:
		{
			printf_spicmd("SPI_GET_TOC\n");
			//printf("SPI_GET_TOC - %d\n",(packet_cmd.data_8[4]) | (packet_cmd.data_8[3]<<8) );
			u32 toc_gd[102];

			//toc - dd/sd
			libGDR_GetToc(&toc_gd[0],packet_cmd.data_8[1]&0x1);

			gd_spi_pio_end((u8*)&toc_gd[0], (packet_cmd.data_8[4]) | (packet_cmd.data_8[3]<<8) );
		}
		break;

		//mount/map drive ? some kind of reset/unlock ??
		//seems like a non data command :)
	case 0x70:
		printf_spicmd("SPI : unkown ? [0x70]\n");
		/*GDStatus.full=0x50; //FIXME
		RaiseInterrupt(holly_GDROM_CMD);*/

		gd_set_state(gds_procpacketdone);
		break;
	case 0x71:
		{
			printf_spicmd("SPI : unkown ? [0x71]\n");
			//printf("SPI : unkown ? [0x71]\n");
	
			gd_spi_pio_end((u8*)&gd_data_0x71[0],gd_data_0x71_len);//uCount

			if (libGDR_GetDiscType()==GdRom || libGDR_GetDiscType()==CdRom_XA)
				SecNumber.Status=GD_PAUSE;
			else
				SecNumber.Status=GD_STANDBY;
		}
		break;
	case SPI_SET_MODE:
		{
			printf_spicmd("SPI_SET_MODE\n");
			u32 Offset = packet_cmd.data_8[2];
			u32 Count = std::min((u32)packet_cmd.data_8[4], 10 - Offset);	// limit to writable area
			set_mode_offset=Offset;
			gd_spi_pio_read_end(Count,gds_process_set_mode);
		}

		break;


	case SPI_REQ_STAT:
		{
			printf_spicmd("SPI_REQ_STAT\n");
			printf("GDROM: Unhandled Sega SPI frame: SPI_REQ_STAT\n");
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(cdda.CurrAddr.FAD, elapsed);
			u8 stat[10];

			//0  0   0   0   0   STATUS
			stat[0]=SecNumber.Status;   //low nibble 
			//1 Disc Format Repeat Count
			stat[1]=(u8)(SecNumber.DiscFormat<<4) | (cdda.repeats);
			//2 Address Control
			stat[2] = (SecNumber.DiscFormat == 0 ? 0 : 0x40) | 1; // Control = 4 for data track
			//3 TNO
			stat[3] = tracknum;
			//4 X
			stat[4] = 1;
			//5 FAD
			stat[5]=cdda.CurrAddr.B0;
			//6 FAD
			stat[6]=cdda.CurrAddr.B1;
			//7 FAD
			stat[7]=cdda.CurrAddr.B2;
			//8 Max Read Error Retry Times
			stat[8]=0;
			//9 0   0   0   0   0   0   0   0
			stat[9]=0;

			gd_spi_pio_end(&stat[packet_cmd.data_8[2]],packet_cmd.data_8[4]);
		}
		break;

	case SPI_REQ_ERROR:
		printf_spicmd("SPI_REQ_ERROR\n");
		printf("GDROM: Unhandled Sega SPI frame: SPI_REQ_ERROR\n");

		u8 resp[10];
		resp[0]=0xF0;
		resp[1]=0;
		resp[2]= SecNumber.Status==GD_BUSY ? 2:0;//sense
		resp[3]=0;
		resp[4]=resp[5]=resp[6]=resp[7]=0; //Command Specific Information
		resp[8]=0;//Additional Sense Code
		resp[9]=0;//Additional Sense Code Qualifier

		gd_spi_pio_end(resp,packet_cmd.data_8[4]);
		break;

	case SPI_REQ_SES:
		printf_spicmd("SPI_REQ_SES\n");

		u8 ses_inf[6];
		libGDR_GetSessionInfo(ses_inf,packet_cmd.data_8[2]);
		ses_inf[0]=SecNumber.Status;
		gd_spi_pio_end((u8*)&ses_inf[0],packet_cmd.data_8[4]);
		break;

	case SPI_CD_OPEN:
		printf_spicmd("SPI_CD_OPEN\n");
		printf("GDROM: Unhandled Sega SPI frame: SPI_CD_OPEN\n");


		gd_set_state(gds_procpacketdone);
		break;

	case SPI_CD_PLAY:
		{
			const u32 param_type = packet_cmd.data_8[1] & 7;
			printf_spicmd("SPI_CD_PLAY param_type=%d", param_type);

			if (param_type == 1 || param_type == 2)
			{
				cdda.status = cdda_t::Playing;
				SecNumber.Status = GD_PLAY;

				bool min_sec_frame = param_type == 2;
				cdda.StartAddr.FAD = cdda.CurrAddr.FAD = GetFAD(&packet_cmd.data_8[2], min_sec_frame);
				cdda.EndAddr.FAD = GetFAD(&packet_cmd.data_8[8], min_sec_frame);
				if (cdda.EndAddr.FAD == 0)
				{
					// Get the last sector of the disk
					u8 ses_inf[6] = {};
					libGDR_GetSessionInfo(ses_inf, 0);

					cdda.EndAddr.FAD = ses_inf[3] << 16 | ses_inf[4] << 8 | ses_inf[5];
				}
				cdda.repeats = packet_cmd.data_8[6] & 0xF;
				GDStatus.DSC = 1;
			}
			else if (param_type == 7)
			{
				if (cdda.status == cdda_t::Paused)
				{
					// Resume from previous pos unless we're at the end
					if (cdda.CurrAddr.FAD > cdda.EndAddr.FAD)
					{
						cdda.status = cdda_t::Terminated;
						SecNumber.Status = GD_STANDBY;
					}
					else
					{
						cdda.status = cdda_t::Playing;
						SecNumber.Status = GD_PLAY;
					}
				}
			}
			else
				die("SPI_CD_PLAY: unknown parameter");

			/*DEBUG_LOG(GDROM, "CDDA StartAddr=%d EndAddr=%d repeats=%d status=%d CurrAddr=%d",cdda.StartAddr.FAD,
					cdda.EndAddr.FAD, cdda.repeats, cdda.status, cdda.CurrAddr.FAD);*/

			gd_set_state(gds_procpacketdone);
		}
		break;

	case SPI_CD_SEEK:
		{
			printf_spicmd("SPI_CD_SEEK\n");
			printf("GDROM: Unhandled Sega SPI frame: SPI_CD_SEEK\n");

			SecNumber.Status=GD_PAUSE;
			if (cdda.status == cdda_t::Playing)
				cdda.status = cdda_t::Paused;

			u32 param_type=packet_cmd.data_8[1]&0x7;
			printf("param_type=%d\n",param_type);
			if (param_type==1)
			{
				cdda.StartAddr.FAD=cdda.CurrAddr.FAD=GetFAD(&packet_cmd.data_8[2],0);
				GDStatus.DSC=1;	//we did the seek xD lol
			}
			else if (param_type==2)
			{
				cdda.StartAddr.FAD=cdda.CurrAddr.FAD=GetFAD(&packet_cmd.data_8[2],1);
				GDStatus.DSC=1;	//we did the seek xD lol
			}
			else if (param_type==3)
			{
				//stop audio , goto home
				SecNumber.Status=GD_STANDBY;
				cdda.StartAddr.FAD=cdda.CurrAddr.FAD=150;
				GDStatus.DSC=1;	//we did the seek xD lol
			}
			else if (param_type==4)
			{
				//pause audio -- nothing more
			}
			else
			{
				die("SPI_CD_SEEK  : not known parameter..");
			}

			printf("cdda.StartAddr=%d\n",cdda.StartAddr.FAD);
			printf("cdda.EndAddr=%d\n",cdda.EndAddr.FAD);
			printf("cdda.repeats=%d\n",cdda.repeats);
			printf("cdda.playing=%d\n",cdda.status);
			printf("cdda.CurrAddr=%d\n",cdda.CurrAddr.FAD);


			gd_set_state(gds_procpacketdone);
		}
		break;

	case SPI_CD_SCAN:
		printf_spicmd("SPI_CD_SCAN\n");
		printf("GDROM: Unhandled Sega SPI frame: SPI_CD_SCAN\n");


		gd_set_state(gds_procpacketdone);
		break;

	case SPI_GET_SCD:
		{
			printf_spicmd("SPI_GET_SCD\n");
			//printf("\nGDROM:\tUnhandled Sega SPI frame: SPI_GET_SCD\n");

			const u32 format = packet_cmd.data_8[1] & 0xF;
			const u32 alloc_len = (packet_cmd.data_8[3] << 8) | packet_cmd.data_8[4];
			u8 subc_info[100];
			u32 size = gd_get_subcode(format, read_params.start_sector - 1, subc_info);
			gd_spi_pio_end(subc_info, std::min(size, alloc_len));
		}
		break;

	default:
		printf("GDROM: Unhandled Sega SPI frame: %X\n", packet_cmd.data_8[0]);

		gd_set_state(gds_procpacketdone);
		break;
	}
}
//Read handler
u32 ReadMem_gdrom(u32 Addr, u32 sz)
{
	switch (Addr)
	{
		//cancel interrupt
	case GD_STATUS_Read :
		asic_CancelInterrupt(holly_GDROM_CMD);	//Clear INTRQ signal
		printf_rm("GDROM: STATUS [cancel int](v=%X)\n",GDStatus.full);
		return GDStatus.full | (1<<4);

	case GD_ALTSTAT_Read:
		printf_rm("GDROM: Read From AltStatus (v=%X)\n",GDStatus.full);
		return GDStatus.full | (1<<4);

	case GD_BYCTLLO	:
		printf_rm("GDROM: Read From GD_BYCTLLO\n");
		return ByteCount.low;

	case GD_BYCTLHI	:
		printf_rm("GDROM: Read From GD_BYCTLHI\n");
		return ByteCount.hi;


	case GD_DATA:
		if(2!=sz)
			printf("GDROM: Bad size on DATA REG Read\n");

		if (gd_state == gds_pio_send_data)
		{
			if (pio_buff.index==pio_buff.size)
				printf("GDROM: Illegal Read From DATA (underflow)\n");
			else
			{
				u32 rv= HOST_TO_LE16(pio_buff.data[pio_buff.index]);
				pio_buff.index+=1;
				ByteCount.Set(ByteCount.Get()-2);
				if (pio_buff.index==pio_buff.size)
				{
					verify(pio_buff.next_state != gds_pio_send_data);
					//end of pio transfer !
					gd_set_state(pio_buff.next_state);
				}
				return rv;
			}

		}
		else
			printf("GDROM: Illegal Read From DATA (wrong mode)\n");

		return 0;

	case GD_DRVSEL:
		printf_rm("GDROM: Read From DriveSel\n");
		return DriveSel;

	case GD_ERROR_Read:
		printf_rm("GDROM: Read from ERROR Register\n");
		return Error.full;

	case GD_IREASON_Read:
		printf_rm("GDROM: Read from INTREASON Register\n");
		return IntReason.full;

	case GD_SECTNUM:
		printf_rm("GDROM: Read from SecNumber Register (v=%X)\n", SecNumber.full);
		return SecNumber.full;

	default:
		printf("GDROM: Unhandled Read From %X sz:%X\n",Addr,sz);
		return 0;
	}
}

//Write Handler
void WriteMem_gdrom(u32 Addr, u32 data, u32 sz)
{
	switch(Addr)
	{
	case GD_BYCTLLO:
		printf_rm("GDROM: Write to GD_BYCTLLO = %X sz:%X\n",data,sz);
		ByteCount.low =(u8) data;
		break;

	case GD_BYCTLHI:
		printf_rm("GDROM: Write to GD_BYCTLHI = %X sz:%X\n",data,sz);
		ByteCount.hi =(u8) data;
		break;

	case GD_DATA:
		{
			if(2!=sz)
				printf("GDROM: Bad size on DATA REG\n");
			if (gd_state == gds_waitpacket)
			{
				packet_cmd.data_16[packet_cmd.index]=HOST_TO_LE16((u16)data);
				packet_cmd.index+=1;
				if (packet_cmd.index==6)
					gd_set_state(gds_procpacket);
			}
			else if (gd_state == gds_pio_get_data)
			{
				pio_buff.data[pio_buff.index]=HOST_TO_LE16((u16)data);
				pio_buff.index+=1;
				if (pio_buff.size==pio_buff.index)
				{
					verify(pio_buff.next_state!=gds_pio_get_data);
					gd_set_state(pio_buff.next_state);
				}
			}
			else
				printf("GDROM: Illegal Write to DATA\n");
			return;
		}
	case GD_DEVCTRL_Write:
		printf("GDROM: Write GD_DEVCTRL ( Not implemented on dc)\n");
		break;

	case GD_DRVSEL:
		printf("GDROM: Write to GD_DRVSEL\n");
		DriveSel = data;
		break;


		//	By writing "3" as Feature Number and issuing the Set Feature command,
		//	the PIO or DMA transfer mode set in the Sector Count register can be selected.
		//	The actual transfer mode is specified by the Sector Counter Register.

	case GD_FEATURES_Write:
		printf_rm("GDROM: Write to GD_FEATURES\n");
		Features.full =(u8) data;
		break;

	case GD_SECTCNT_Write:
		printf("GDROM: Write to SecCount = %X\n", data);
		SecCount.full =(u8) data;
		break;

	case GD_SECTNUM	:
		printf("GDROM: Write to SecNum; not possible = %X\n", data);
		break;

	case GD_COMMAND_Write:
		verify(sz==1);
		if ((data !=ATA_NOP) && (data != ATA_SOFT_RESET))
			verify(gd_state==gds_waitcmd);
		//printf("\nGDROM:\tCOMMAND: %X !\n", data);
		ata_cmd.command=(u8)data;
		gd_set_state(gds_procata);
		break;

	default:
		printf("\nGDROM:\tUnhandled Write to %X <= %X sz:%X\n",Addr,data,sz);
		break;
	}
}

//is this needed ?
void UpdateGDRom()
{
	if(!(SB_GDST&1) || !(SB_GDEN &1))
		return;

	//SB_GDST=0;

	//TODO : Fix dmaor
	u32 dmaor	= DMAC_DMAOR.full;

	u32	src		= SB_GDSTARD,
		len		= SB_GDLEN-SB_GDLEND ;

	len=min(len,(u32)32000);
	// do we need to do this for gdrom dma ?
	if(0x8201 != (dmaor &DMAOR_MASK)) {
		printf("\n!\tGDROM: DMAOR has invalid settings (%X) !\n", dmaor);
		//return;
	}
	if(len & 0x1F) {
		printf("\n!\tGDROM: SB_GDLEN has invalid size (%X) !\n", len);
		return;
	}

	if(0 == len)
	{
		printf("\n!\tGDROM: Len: %X, Abnormal Termination !\n", len);
	}
	u32 len_backup=len;
	if( 1 == SB_GDDIR )
	{
		while(len)
		{
			u32 buff_size =read_buff.cache_size - read_buff.cache_index;
			if (buff_size==0)
			{
				verify(read_params.remaining_sectors>0);
				//buffer is empty , fill it :)
				FillReadBuffer();
			}

			//transfer up to len bytes
			if (buff_size>len)
			{
				buff_size=len;
			}
			
			WriteMemBlock_nommu_ptr(src,(u32*)&read_buff.cache[read_buff.cache_index], buff_size);
			//Can't use it due to endianess mess :d
			/*{
				u32* pdata=(u32*)&read_buff.cache[read_buff.cache_index];
				for (u32 i=0;i<buff_size;i+=4)
				{
					WriteMem32_nommu(src+i,HOST_TO_LE32(pdata[i>>2]));
				}
			}*/

			read_buff.cache_index+=buff_size;
			src+=buff_size;
			len-=buff_size;
		}
	}
	else
		msgboxf("GDROM: SB_GDDIR %X (TO AICA WAVE MEM?)",MBX_ICONERROR, SB_GDDIR);

	//SB_GDLEN = 0x00000000; //13/5/2k7 -> acording to docs these regs are not updated by hardware
	//SB_GDSTAR = (src + len_backup);

	SB_GDLEND+= len_backup;
	SB_GDSTARD+= len_backup;//(src + len_backup)&0x1FFFFFFF;

	if (SB_GDLEND==SB_GDLEN)
	{
		//printf("Streamed GDMA end - %d bytes trasnfered\n",SB_GDLEND);
		SB_GDST=0;//done
		// The DMA end interrupt flag
		asic_RaiseInterrupt(holly_GDROM_DMA);
	}
	//Readed ALL sectors
	if (read_params.remaining_sectors==0)
	{
		u32 buff_size =read_buff.cache_size - read_buff.cache_index;
		//And all buffer :p
		if (buff_size==0)
		{
			verify(!(SB_GDST&1))
			gd_set_state(gds_procpacketdone);
		}
	}
}
//Dma Start
void GDROM_DmaStart(u32 data)
{
	if (SB_GDEN==0)
	{
		printf("Invalid GD-DMA start, SB_GDEN=0.Ingoring it.\n");
		return;
	}
	SB_GDST|=data&1;

	if (SB_GDST==1)
	{
		SB_GDSTARD=SB_GDSTAR;
		SB_GDLEND=0;
		//printf("Streamed GDMA start\n");
		UpdateGDRom();
	}
}


void GDROM_DmaEnable(u32 data)
{
	SB_GDEN=data&1;
	if (SB_GDEN==0 && SB_GDST==1)
	{
		printf("GD-DMA aborted\n");
		SB_GDST=0;
	}
}
//Init/Term/Res
void gdrom_reg_Init()
{
	sb_regs[(SB_GDST_addr-SB_BASE)>>2].flags=REG_32BIT_READWRITE | REG_READ_DATA;
	sb_regs[(SB_GDST_addr-SB_BASE)>>2].writeFunction=GDROM_DmaStart;

	sb_regs[(SB_GDEN_addr-SB_BASE)>>2].flags=REG_32BIT_READWRITE | REG_READ_DATA;
	sb_regs[(SB_GDEN_addr-SB_BASE)>>2].writeFunction=GDROM_DmaEnable;
}
void gdrom_reg_Term()
{

}

void gdrom_reg_Reset(bool Manual)
{
	SB_GDST = 0;
	SB_GDEN = 0;
	// set default hardware information
	memset(&GD_HardwareInfo, 0, sizeof(GD_HardwareInfo));
	GD_HardwareInfo.speed = 0x0;
	GD_HardwareInfo.standby_hi = 0x00;
	GD_HardwareInfo.standby_lo = 0xb4;
	GD_HardwareInfo.read_flags = 0x19;
	GD_HardwareInfo.read_retry = 0x08;
	memcpy(GD_HardwareInfo.drive_info, "SE      ", sizeof(GD_HardwareInfo.drive_info));
	memcpy(GD_HardwareInfo.system_version, "Rev 6.43", sizeof(GD_HardwareInfo.system_version));
	memcpy(GD_HardwareInfo.system_date, "990408", sizeof(GD_HardwareInfo.system_date));
}

