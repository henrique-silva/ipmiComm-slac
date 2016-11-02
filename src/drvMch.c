#include <stdint.h>
#include <stdio.h>   /* fopen, etc. */
#include <string.h>
#include <math.h>    /* floor, pow */
#include <ctype.h>   /* toupper */

#include <arpa/inet.h>
#include <netinet/in.h>

#include <errlog.h>
#include <epicsExport.h>
#include <drvSup.h>
#include <iocsh.h>
#include <epicsMutex.h>
#include <cantProceed.h>
#include <epicsThread.h>
#include <dbScan.h>
#include <registryFunction.h>
#include <registry.h>

#include <drvMch.h>
#include <drvMchMsg.h>
#include <ipmiMsg.h>
#include <picmgDef.h>

#undef DEBUG 
#define DEBUG

#define PING_PERIOD  5

const void *mchCbRegistryId = (void*)&mchCbRegistryId;

char mchDescString[MCH_TYPE_MAX][MCH_DESC_MAX_LENGTH] =
                {   "Unknown device\0", 
                    "MicroTCA Crate, VT MCH\0",
                    "MicroTCA Crate, NAT MCH\0",
                    "Supermicro Server\0",
                    "ATCA Crate, Pentair",
                    "ATCA Crate, Artesyn",
                    "",
                    "",
                    "",
                    ""
                };

int mchCounter = 0;

epicsMutexId mchStatMtx[MAX_MCH];
uint32_t     mchStat[MAX_MCH] = { 0 };

/* change some of these to static! */
static int mchSdrGetDataAll(MchData mchData);
static int mchFruGetDataAll(MchData mchData);
int mchGetFruLkup(MchData mchData, int index);
static int  mchCnfg(MchData mchData);


static void
mchSeqInit(IpmiSess ipmiSess)
{
int i;
	/* Initialize IPMI sequence */
	ipmiSess->seq = 0;

        /* Initialize our stored sequence number for messages from MCH */
	ipmiSess->seqRply[0] = IPMI_WRAPPER_SEQ_INITIAL;
        for ( i = 1; i < IPMI_RPLY_SEQ_LENGTH - 1 ; i++)
                ipmiSess->seqRply[i] = 0;	

	for ( i = 0; i < IPMI_WRAPPER_SEQ_LENGTH ; i++)
	        ipmiSess->seqSend[i] = 0;

	for ( i = 0; i < IPMI_WRAPPER_ID_LENGTH ; i++)
		ipmiSess->id[i] = 0;
}

static void
mchSetAuth(MchSess mchSess, IpmiSess ipmiSess, uint8_t authByte)
{
	ipmiSess->authReq = 0xFF; /* Default to illegal value */
	ipmiSess->authSup = IPMI_AUTH_TYPE_SUPPORT( authByte );

	if ( ipmiSess->authSup & (1<<IPMI_MSG_AUTH_TYPE_NONE) ) {
		ipmiSess->authReq = IPMI_MSG_AUTH_TYPE_NONE;
	}
	else if (  ipmiSess->authSup & (1<<IPMI_MSG_AUTH_TYPE_PWD_KEY) ) {
		ipmiSess->authReq = IPMI_MSG_AUTH_TYPE_PWD_KEY;
	}
	else
		printf("mchSetAuth: No supported auth type for %s, supported mask is 0x%02x\n", 
		    mchSess->name, ipmiSess->authSup);
}

/* Start communication session with MCH
 * Multi-step handshaking sequence
 *
 * Caller must perform locking.
 *
 *   RETURNS:
 *         0 on success
 *         non-zero on failure
 */		
static int 
mchCommStart(MchSess mchSess, IpmiSess ipmiSess)
{	
uint8_t response[MSG_MAX_LENGTH] = { 0 };
int     i, dbg = MCH_DBG( mchStat[mchSess->instance] );

	if ( dbg )
		printf("%s Connecting...\n", mchSess->name);

	mchSeqInit( ipmiSess );

	if ( mchMsgGetChanAuth( mchSess, ipmiSess, response ) ) {
		if ( dbg )
			printf("%s Get Channel Authentication failed\n", mchSess->name);
		return -1;	
	}

	mchSetAuth( mchSess, ipmiSess, response[IPMI_RPLY_IMSG2_AUTH_CAP_AUTH_OFFSET] );

	if ( mchMsgGetSess( mchSess, ipmiSess, response ) ) {
		if ( dbg )
			printf("%s Get Session failed\n", mchSess->name);
		return -1;
	}

        /* Extract temporary session ID */
        for ( i = 0; i < IPMI_RPLY_IMSG2_SESSION_ID_LENGTH ; i++)
                ipmiSess->id[i] = response[IPMI_RPLY_IMSG2_GET_SESS_TEMP_ID_OFFSET + i];

        /* Extract challenge string */
        for ( i = 0; i < IPMI_RPLY_CHALLENGE_STR_LENGTH ; i++)                
		ipmiSess->str[i] = response[IPMI_RPLY_IMSG2_GET_SESS_CHALLENGE_STR_OFFSET + i];

	if ( mchMsgActSess( mchSess, ipmiSess, response ) ) {
		if ( dbg )
			printf("%s Activate session failed\n", mchSess->name);
		goto bail;;
	}

        /* Extract session ID */
        for ( i = 0; i < IPMI_RPLY_IMSG2_SESSION_ID_LENGTH ; i++)
                ipmiSess->id[i] = response[IPMI_RPLY_IMSG2_ACT_SESS_ID_OFFSET + i];

        /* Extract initial sequence number for messages to MCH */
        for ( i = 0; i < IPMI_RPLY_INIT_SEND_SEQ_LENGTH ; i++)
                ipmiSess->seqSend[i] = response[IPMI_RPLY_IMSG2_ACT_SESS_INIT_SEND_SEQ_OFFSET + i];

	/* Need a non-hard-coded way to determine privelige level */
	if ( mchMsgSetPriv( mchSess, ipmiSess, response, IPMI_MSG_PRIV_LEVEL_OPER ) ) {
		if ( dbg )
			printf("%s Set session privilege failed\n", mchSess->name);
		goto bail;
	}

	return 0;

bail:
	mchMsgCloseSess( mchSess, ipmiSess, response );
	return -1;

}

/* Start new session with MCH
 * Reset session sequence number and session ID to 0,
 * then call mchCommStart to initiate new session
 *
 * Caller must perform locking.
 *
 *   RETURNS: (return val from mchCommStart)
 *           0 on success
 *           non-zero on failure          
 */
int
mchNewSession(MchSess mchSess, IpmiSess ipmiSess)
{
int rval = -1;

	mchSeqInit( ipmiSess );

	if ( MCH_ONLN( mchStat[mchSess->instance] ) )
		rval = mchCommStart( mchSess, ipmiSess );

	return rval;
}

static uint8_t
bcdPlusConvert(uint8_t raw)
{

	switch( raw ) {

		default:
			printf("Illegal or reserved BCD PLUS byte 0x%02x\n", raw);
		case 0:
			return '0';
		case 1:
			return '1';
			break;
		case 2:
			return '2';
			break;
		case 3:
			return '3';
			break;
		case 4:
			return '4';
			break;
		case 5:
			return '5';
			break;
		case 6:
			return '6';
			break;
		case 7:
			return '7';
			break;
		case 8:
			return '8';
			break;
		case 9:
			return '9';
			break;
		case 0xA:
			return ' ';
			break;
		case 0xB:
			return '-';
			break;
		case 0xC:
			return '.';
			break;
	}
}

static void
sixBitAsciiConvert(uint8_t *input, uint8_t *output, uint8_t n_input, uint8_t n_output)
{
int i = 0, j = 0;

	while( 1 ) {

			output[i++] = (input[j*3 + 0] & 0x3F) + 0x20;
			if ( i >= n_output)
				break;

			output[i++] = (((input[j*3 + 0] & 0xC0) >> 6) | ((input[j*3 + 1] & 0xF) << 2)) + 0x20;
			if ( i >= n_output)
				break;

			output[i++] = (((input[j*3 + 1] & 0xF0) >> 4) | ((input[j*3 + 2] & 0x3) << 4)) + 0x20;
			if ( i >= n_output)
				break;

			output[i++] = ((input[j*3 + 2] & 0xFC) >> 2) + 0x20;
			if ( i >= n_output)
				break;

			j++;
	}
}

static void
hexAsciiConvert(uint8_t *input, uint8_t *output, uint8_t n_output)
{
int i, j = 0, k;
uint8_t a;

	for ( i = 0; i < n_output/2; i++ ) {

		for ( k = 1; k > -1; k-- ) {

			a = (input[i] & (0xF<<(k*4))) >> k*4;

			if ( a == 0 )
				output[j] = '0';
			else if ( a == 1 ) 
				output[j] = '1';
			else if ( a == 2 ) 
				output[j] = '2';
			else if ( a == 3 ) 
				output[j] = '3';
			else if ( a == 4 ) 
				output[j] = '4';
			else if ( a == 5 ) 
				output[j] = '5';
			else if ( a == 6 ) 
				output[j] = '6';
			else if ( a == 7 ) 
				output[j] = '7';
			else if ( a == 8 ) 
				output[j] = '8';
			else if ( a == 9 ) 
				output[j] = '9';
			else if ( a == 10 ) 
				output[j] = 'A';
			else if ( a == 11 ) 
				output[j] = 'B';
			else if ( a == 12 ) 
				output[j] = 'C';
			else if ( a == 13 ) 
				output[j] = 'D';
			else if ( a == 14 ) 
				output[j] = 'E';
			else if ( a == 15 ) 
				output[j] = 'F';

			j++;
		} 
	}
}

/* If error, set field->length to zero to indicate no valid data */
static int
mchFruFieldConvertData(FruField field, uint8_t lang)
{
int i;

	switch( field->type ) {

	default:
			printf("FRU field data type %i is not supported\n", field->type);
			return -1;

		case FRU_DATA_TYPE_BINARY:
			printf("\nFRU field data type binary or unspecified. Add support!\n");
			field->length = 2*field->rlength;
printf("field length %i\n", field->length);
			if ( !( field->data = ipmiReallocZeros( field->data, field->length ) ) ) {
				printf("No memory for FRU field data\n");
				field->length = 0;
				return -1;
			}
			else {
printf("field data: ");
for ( i = 0; i < field->rlength; i++ )
printf("0x%02x ", field->rdata[i]);
printf("\n");
				hexAsciiConvert( field->rdata, field->data, field->length );
				return 0;
			}


		case FRU_DATA_TYPE_BCDPLUS:

			field->length = field->rlength;
			if ( !( field->data = ipmiReallocZeros( field->data, field->length ) ) ) {
				printf("No memory for FRU field data\n");
				field->length = 0;
				return -1;
			}
			else {
				for ( i = 0; i < field->length; i++ )
					field->data[i] = bcdPlusConvert( field->rdata[i] );
				return 0;
			}

		case FRU_DATA_TYPE_6BITASCII:

			field->length = ceil( 8*((float)field->rlength/6) );
			if ( field->rlength % 3 )
				printf("6-bit ASCII data length not integer multiple of 3, something may be wrong.\n");

			if ( !( field->data = ipmiReallocZeros( field->data, field->length ) ) ) {
				printf("No memory for FRU field data\n");
				field->length = 0;
				return -1;
			}
			else {
				sixBitAsciiConvert( field->rdata, field->data, field->rlength, field->length);
				return 0;
			}

		case FRU_DATA_TYPE_LANG:

			if ( IPMI_DATA_LANG_ENGLISH( lang ) ) {

				field->length = field->rlength;
				if ( !( field->data = ipmiReallocZeros( field->data, field->length ) ) ) {
					printf("No memory for FRU field data\n");
					field->length = 0;
					return -1;
				}

				memcpy( field->data, field->rdata, field->length );
				return 0;
			}
			else
				printf("Warning FRU data language %i is not english; need to add 2-byte unicode support\n", lang);		   
	}
	return 0;
}

/* Copy FRU area field to data structure
 *
 * Caller must perform locking.
 *
 *   RETURNS:
 *           0 on success
 *          -1 if no data to store
 */
static int
mchFruFieldGet(FruField field, uint8_t *raw, unsigned *offset, uint8_t lang)
{
int i;
        if ( ( field->rlength = IPMI_DATA_LENGTH( raw[*offset] ) ) ) {

                field->type = IPMI_DATA_TYPE( raw[*offset] );

                (*offset)++;

                if ( !( field->rdata = ipmiReallocZeros( field->rdata, field->rlength ) ) )
                        printf("No memory for FRU field data\n");
                else {
                        for ( i = 0; i < field->rlength; i++ )
                                field->rdata[i] = raw[*offset + i];

                        *offset += field->rlength;

                        return mchFruFieldConvertData( field, lang );
                }
        }
        return -1;
}

/* 
 * Copy FRU Chassis area data to chassis structure
 *
 * Caller must perform locking.
 */
static void
mchFruChassisDataGet(FruChassis chas, uint8_t *raw, unsigned *offset)
{
	if ( 0 != (*offset = 8*raw[FRU_DATA_COMMON_HEADER_OFFSET + FRU_DATA_COMMON_HEADER_CHASSIS_AREA_OFFSET]) ) {

		chas->type = raw[*offset + FRU_DATA_CHASSIS_TYPE_OFFSET];

		*offset += FRU_DATA_CHASSIS_AREA_PART_LENGTH_OFFSET;

		if ( mchFruFieldGet( &(chas->part),   raw, offset, IPMI_DATA_LANG_CODE_ENGLISH1 ) )
			(*offset)++;
		if ( mchFruFieldGet( &(chas->sn),     raw, offset, IPMI_DATA_LANG_CODE_ENGLISH1 ) )
			(*offset)++;
	}
}

/* 
 * Copy FRU Product area data to product structure
 *
 * Caller must perform locking.
 */
static void
mchFruProdDataGet(FruProd prod, uint8_t *raw, unsigned *offset)
{
	if ( 0 != (*offset = 8*raw[FRU_DATA_COMMON_HEADER_OFFSET + FRU_DATA_COMMON_HEADER_PROD_AREA_OFFSET]) ) {

		prod->lang = raw[*offset + FRU_DATA_BOARD_AREA_LANG_OFFSET];

		*offset += FRU_DATA_PROD_AREA_MANUF_LENGTH_OFFSET;

		if ( mchFruFieldGet( &(prod->manuf),   raw, offset, prod->lang ) )
			(*offset)++;
		if ( mchFruFieldGet( &(prod->prod),    raw, offset, prod->lang) )
			(*offset)++;
		if ( mchFruFieldGet( &(prod->part),    raw, offset, prod->lang ) )
			(*offset)++;
		if ( mchFruFieldGet( &(prod->version), raw, offset, prod->lang ) )
			(*offset)++;
		if ( mchFruFieldGet( &(prod->sn),      raw, offset, prod->lang ) )
			(*offset)++;
	}
}

/* 
 * Copy FRU Board area data to board structure
 *
 * Caller must perform locking.
 */
static void
mchFruBoardDataGet(FruBoard board, uint8_t *raw, unsigned *offset)
{
	if ( 0 != (*offset = 8*raw[FRU_DATA_COMMON_HEADER_OFFSET + FRU_DATA_COMMON_HEADER_BOARD_AREA_OFFSET] ) ) {

		board->lang = raw[*offset + FRU_DATA_BOARD_AREA_LANG_OFFSET];

		*offset += FRU_DATA_BOARD_AREA_MANUF_LENGTH_OFFSET;

		if ( mchFruFieldGet( &(board->manuf), raw, offset, board->lang ) )
			(*offset)++;
		if ( mchFruFieldGet( &(board->prod),  raw, offset, board->lang ) )
			(*offset)++;
		if ( mchFruFieldGet( &(board->sn),    raw, offset, board->lang ) )
			(*offset)++;
		if ( mchFruFieldGet( &(board->part),  raw, offset, board->lang ) )
			(*offset)++;	       
	}
}

/* 
 * Get data for one FRU
 *
 * Read FRU inventory info. If error in response
 * or FRU data area size is zero, return. Else read FRU data
 * and call mchFru*DataGet to store in FRU structure.
 *
 * Caller must perform locking.
 */
static int
mchFruDataGet(MchData mchData, Fru fru) 
{
MchSess    mchSess = mchData->mchSess;
int        inst    = mchSess->instance;
uint8_t    response[MSG_MAX_LENGTH] = { 0 };
uint8_t   *raw = 0; 
int        i;
uint16_t   sizeInt;  /* Size of FRU data area in bytes */
unsigned   nread;    /* Number of FRU data reads */
unsigned   offset;   /* Offset into FRU data */
int        rval;

	/* Get FRU Inventory Info */
	if ( mchMsgGetFruInvInfoWrapper( mchData, response, fru ) )
		return -1;

	fru->size[0] = response[IPMI_RPLY_IMSG2_FRU_AREA_SIZE_LSB_OFFSET];
	fru->size[1] = response[IPMI_RPLY_IMSG2_FRU_AREA_SIZE_MSB_OFFSET];
	fru->access  = response[IPMI_RPLY_IMSG2_FRU_AREA_ACCESS_OFFSET];  

	if ( 0 == (sizeInt = arrayToUint16( fru->size )) )
		return 0;

	if ( MCH_DBG( mchStat[inst] ) )
		printf("%s mchFruDataGet: FRU addr 0x%02x ID %i inventory info size %i\n", mchSess->name, fru->sdr.addr, fru->sdr.fruId, sizeInt);

	if ( !(raw = ipmiReallocZeros( raw, sizeInt ) ) ) {
		printf("mchFruDataGet: No memory for FRU addr 0x%02x ID %i data\n", fru->sdr.addr, fru->sdr.fruId);
		return -1;
	}

	/* Too many reads! But no way to determine the end of the data until we read it, so for now... */
	if ( ( nread = floor( sizeInt/MSG_FRU_DATA_READ_SIZE ) ) > 50 )
		nread = 50;

	/* Initialize read offset to 0 */
	for ( i = 0; i < sizeof( fru->readOffset ); i ++ )
		fru->readOffset[i] = 0;

	/* Read FRU data, store in raw, increment our read offset for next read. If error returned for requested FRU ID, abort */
	for ( i = 0; i < nread; i++ ) {
		fru->read = i;

		rval = mchMsgReadFruWrapper( mchData, response, fru, fru->readOffset, MSG_FRU_DATA_READ_SIZE );
		if ( rval ) {
			if ( IPMI_COMP_CODE_REQUESTED_DATA == rval )
				break;
		}
		else
			memcpy( raw + i*MSG_FRU_DATA_READ_SIZE, response + IPMI_RPLY_IMSG2_FRU_DATA_READ_OFFSET, MSG_FRU_DATA_READ_SIZE );
		incr2Uint8Array( fru->readOffset, MSG_FRU_DATA_READ_SIZE );
	}

	if ( MCH_DBG( mchStat[inst] ) >= MCH_DBG_HIGH ) {
		printf("%s FRU addr 0x%02x ID %i raw data, size %i: \n", mchSess->name, fru->sdr.addr, fru->sdr.fruId, sizeInt);
		for ( i = 0; i < sizeInt; i++)
			printf("0x%02x ",raw[i]);
		printf("\n");

		printf("%s FRU addr 0x%02x ID %i raw data, size %i: \n", mchSess->name, fru->sdr.addr, fru->sdr.fruId, sizeInt);
		for ( i = 0; i < sizeInt; i++)
			printf("%c ",raw[i]);
		printf("\n");
	}

	/* Add chassis data get */
	mchFruBoardDataGet( &(fru->board), raw, &offset );
	mchFruProdDataGet(  &(fru->prod) , raw, &offset );
	mchFruChassisDataGet( &(fru->chassis), raw, &offset );
	free( raw );

	return 0;
}


int
mchGetFruLkup(MchData mchData, int index)
{
int i;
	for ( i = 0; i < MAX_FRU_MGMT; i++ ) {
		if ( mchData->mchSys->fruLkup[i] == index )
			return i;
	}
	return -1;
}

/* 
 * Get data for all FRUs. Must be done after SDR data has been stored in FRU struct.
 * If FRU is cooling unit, get fan properties.
 *
 * Caller must perform locking.
 */
static int
mchFruGetDataAll(MchData mchData)
{
MchSess mchSess = mchData->mchSess;
MchSys  mchSys  = mchData->mchSys;
uint8_t i;
Fru fru;
int rval = 0;

	if ( mchData->mchSys->mchcb->assign_site_info )
		mchData->mchSys->mchcb->assign_site_info( mchData );

	if ( mchData->mchSys->mchcb->assign_fru_lkup )
		mchData->mchSys->mchcb->assign_fru_lkup( mchData );

	for ( i = 0; i < mchSys->fruCount ; i++ ) {

	       	fru = &mchSys->fru[i];

		if ( fru->sdr.recType ) {

			if ( mchFruDataGet( mchData, fru ) )
				rval = -1;

			if ( mchData->mchSys->mchcb->fru_data_suppl )
				rval = mchData->mchSys->mchcb->fru_data_suppl( mchData, i );
		}	
	}


	if ( MCH_DBG( mchStat[mchSess->instance] ) >= MCH_DBG_MED ) {
		printf("%s mchFruGetDataAll: FRU Summary:\n", mchSess->name);
		for ( i = 0; i < mchSys->fruCount; i++) {
			fru = &mchSys->fru[i];
			if ( fru->sdr.fruId || (arrayToUint16( fru->size ) > 0) )
				printf("SDR FRU index %i ID %i %s was found, ent id 0x%02x instance 0x%02x, addr 0x%02x id 0x%02x lun %i lkup %i\n", 
				i, fru->sdr.fruId, fru->board.prod.data, fru->sdr.entityId, fru->sdr.entityInst, fru->sdr.addr, fru->sdr.fruId, fru->sdr.lun, mchGetFruLkup( mchData, i));
		}
	}

	return rval;
}

/* 
 * Get sensor/FRU association
 *
 * -Given a sensor index, identify the associated FRU or Management Controller 
 *  and store its identity in the sensor structure. 
 *  For NicroTCA AMC and RTM non-FRU entities, identify the associated 
 *  FRU and do the same. For NAT hot-swap sensors, parse description string 
 *  to find associated FRU ID.
 *
 * Note: these indices refer to our arrays of Sensors and FRUs--
 * they do not refer to the IPMI sensor number or IPMI SDR Record ID
 *
 *
 * Caller must perform locking.
 */
static void
mchSensorGetFru(MchData mchData, uint8_t index)
{
MchSys mchSys = mchData->mchSys;
Sensor sens   = &mchSys->sens[index];

	/* Set default indices to indicate no corresponding fru/mgmt */
	sens->fruId = sens->fruIndex = sens->mgmtIndex = -1; 

	if ( mchSys->mchcb->sensor_get_fru )
		mchSys->mchcb->sensor_get_fru( mchData, sens );
/*** figure out if can decommission sens->fruId in lieu of sens->fruIndex */
}

/* Caller must perform locking */
static int
mchSdrRepGetInfoMsg(MchData mchData, uint8_t *response, uint8_t parm, uint8_t addr) 
{

	return mchMsgGetSdrRepInfoWrapper( mchData, response, parm, addr );

}

/* Caller must perform locking */
static void
mchSdrRepGetTs(uint8_t *response, uint32_t *addTs, uint32_t *delTs) 
{

       	*addTs = ntohl( *(uint32_t *)(response + IPMI_RPLY_IMSG2_SDRREP_ADD_TS_OFFSET) );
       	*delTs = ntohl( *(uint32_t *)(response + IPMI_RPLY_IMSG2_SDRREP_DEL_TS_OFFSET) );
}

/* Compare SDR repository timestamps to detect changes
 *
 * Caller must perform locking
 */
static int
mchSdrRepTsDiff(MchData mchData) {
MchSess   mchSess = mchData->mchSess;
MchSys    mchSys  = mchData->mchSys;
uint32_t  add, del;
uint32_t *addTs = &mchSys->sdrRep.addTs, *delTs = &mchSys->sdrRep.delTs;
uint8_t   buff[MSG_MAX_LENGTH] = { 0 };

	if ( mchSdrRepGetInfoMsg( mchData, buff, IPMI_SDRREP_PARM_GET_SDR, IPMI_MSG_ADDR_BMC ) )
		return 0;

	mchSdrRepGetTs( buff, &add, &del );

	if ( (add != *addTs) || (del != *delTs) ) {

		if ( MCH_DBG( mchStat[mchSess->instance] ) )
			printf("%s SDR rep TS before: 0x%08x 0x%08x, after: 0x%08x 0x%08x\n", mchSess->name, *addTs, *delTs, add, del);

		*addTs = add;
		*delTs = del;

		return -1;
	}

	return 0;
}

/* 
 * Store SDR Repository info 
 *
 * Caller must perform locking.
 */
static int
mchSdrRepGetInfo(MchData mchData, uint8_t parm, uint8_t addr, SdrRep sdrRep, uint32_t *sdrCount)
{
uint8_t response[MSG_MAX_LENGTH] = { 0 };
uint8_t flags;
int     rval;

	rval = mchSdrRepGetInfoMsg( mchData, response, parm, addr );

	if (  parm == IPMI_SDRREP_PARM_GET_SDR ) {

		if ( rval )
			return -1;

		sdrRep->ver     = response[IPMI_RPLY_IMSG2_SDRREP_VER_OFFSET];
		sdrRep->size[0] = response[IPMI_RPLY_IMSG2_SDRREP_CNT_LSB_OFFSET];
		sdrRep->size[1] = response[IPMI_RPLY_IMSG2_SDRREP_CNT_MSB_OFFSET];

		*sdrCount = arrayToUint16( sdrRep->size );

		mchSdrRepGetTs( response, &sdrRep->addTs, &sdrRep->delTs );
	}
	else {

		if ( response[0] ) // completion code
			return -1;

		*sdrCount = sdrRep->devSdrSize = response[IPMI_RPLY_IMSG2_DEV_SDR_INFO_CNT_OFFSET];
		flags = response[IPMI_RPLY_IMSG2_DEV_SDR_INFO_FLAGS_OFFSET];
		sdrRep->devSdrDyn  = DEV_SENSOR_DYNAMIC(flags);
		sdrRep->lun0       = DEV_SENSOR_LUN0(flags);
		sdrRep->lun1       = DEV_SENSOR_LUN1(flags);
		sdrRep->lun2       = DEV_SENSOR_LUN2(flags);
		sdrRep->lun3       = DEV_SENSOR_LUN3(flags);
	}

	return 0;
}

static void
mchStoreAssocDevEntInfo(Entity entity, DevEntAssoc dassoc, uint8_t cntnrAddr, uint8_t id, uint8_t inst, uint8_t cntndAddr, uint8_t cntndChan, int parm, int *count)
{
uint8_t ownerAddr = dassoc->ownerAddr;
uint8_t ownerChan = dassoc->ownerChan;

	if ( parm ) {

		entity[*count].entityId   = id;
		entity[*count].entityInst = inst;

		/* If container address not defined in SDR, use owner address
		 * that was used when querying this FRU or management controller */
		if ( cntnrAddr == 0 ) {
			entity[*count].addr = ownerAddr;
			entity[*count].chan = ownerChan;
		}
		else {
			entity[*count].addr = cntndAddr;
			entity[*count].chan = cntndChan;
		}

#ifdef DEBUG
printf("mchStoreAssocDevEntInfo: Found assoc dev rel entity owner 0x%02x contained id 0x%02x inst 0x%02x index %i entity point addr %i %i\n", 
    entity[*count].addr, entity[*count].entityId, entity[*count].entityInst, *count, (int)(&entity[*count]), (int)entity);
#endif
	}

	(*count)++;
}

static void
mchStoreAssocEntInfo(Entity entity, uint8_t id, uint8_t inst, int parm, int *count)
{

	if ( parm ) {

		entity[*count].entityId   = id;
		entity[*count].entityInst = inst;

	}

	(*count)++;
}

/* Parm of 0 return count of contained entities
 * Parm of 1 stores contained entity info 
 */
static int
mchGetAssocEntInfo(MchSys mchSys, Entity entity, uint8_t addr, uint8_t chan, uint8_t entityId, uint8_t entityInst, int parm)
{
EntAssoc       eassoc;
DevEntAssoc    dassoc;
SdrEntAssoc    esdr;
SdrDevEntAssoc dsdr;
int i, j, range, count = 0;

	for ( i = 0; i < mchSys->devEntAssocCount; i++ ) {
		dassoc = &mchSys->devEntAssoc[i];
		dsdr   = &dassoc->sdr;

printf("dassoc %i dsdr %i\n", dassoc, dsdr);
printf("dassoc->ownerAddr %i dassoc->ownerChan %i\n", dassoc->ownerAddr, dassoc->ownerChan);
printf("dsdr->cntnrAddr %i dsdr->cntnrChan %i\n", dsdr->cntnrAddr, dsdr->cntnrChan );
printf("dsdr->cntnrId %i dsdr->cntnrInst %i\n", dsdr->cntnrId, dsdr->cntnrInst );

		if ( ((addr == dassoc->ownerAddr)  || (addr == dsdr->cntnrAddr)) &&
		     ((chan == dassoc->ownerChan)  || (chan == dsdr->cntnrChan)) &&
		     (entityId   == dsdr->cntnrId) &&
		     (entityInst == dsdr->cntnrInst) ) {
			
			if ( ENTITY_ASSOC_FLAGS_RANGE( dsdr->flags ) ) { /* If contained entities are stored in range */
				
				range = dsdr->cntndInst2 - dsdr->cntndInst1;

				for ( j = 0; j < range; j++ )

					mchStoreAssocDevEntInfo( entity, dassoc, dsdr->cntnrAddr, dsdr->cntndId1, dsdr->cntndInst1 + j, 
								 dsdr->cntndAddr1, dsdr->cntndChan1, parm, &count );

				if ( dsdr->cntndInst2 > 0 ) { /* If second range is also populated */

					range = dsdr->cntndInst4 - dsdr->cntndInst3;

					for ( j = 0; j < range; j++ )

						mchStoreAssocDevEntInfo( entity, dassoc, dsdr->cntnrAddr, dsdr->cntndId1, dsdr->cntndInst2 + j, 
									dsdr->cntndAddr2, dsdr->cntndChan2, parm, &count );
				}
			}
			else { /* If contained entities are stored in list */

				mchStoreAssocDevEntInfo( entity, dassoc, dsdr->cntnrAddr, dsdr->cntndId1, dsdr->cntndInst1, 
								 dsdr->cntndAddr1, dsdr->cntndChan1, parm, &count );

				if ( dsdr->cntndInst2 > 0 ) /* Second in list exists */

					mchStoreAssocDevEntInfo( entity, dassoc, dsdr->cntnrAddr, dsdr->cntndId2, dsdr->cntndInst2, 
								dsdr->cntndAddr2, dsdr->cntndChan2, parm, &count );

				if ( dsdr->cntndInst3 > 0 ) /* Third in list exists */

					mchStoreAssocDevEntInfo( entity, dassoc, dsdr->cntnrAddr, dsdr->cntndId3, dsdr->cntndInst3, 
								dsdr->cntndAddr3, dsdr->cntndChan3, parm, &count );

				if ( dsdr->cntndInst4 > 0 ) /* Fourth in list exists */

					mchStoreAssocDevEntInfo( entity, dassoc, dsdr->cntnrAddr, dsdr->cntndId4, dsdr->cntndInst4, 
								dsdr->cntndAddr4, dsdr->cntndChan4, parm, &count );
			}

			if ( 0 == ENTITY_ASSOC_FLAGS_LINK( dassoc->sdr.flags ) )
				break; /* Should be no further assocated entities for this container */
		}
       	}

	for ( i = 0; i < mchSys->entAssocCount; i++ ) {
		eassoc = &mchSys->entAssoc[i];
		esdr   = &eassoc->sdr;

		if ( (entityId   == eassoc->sdr.cntnrId) &&
		     (entityInst == eassoc->sdr.cntnrInst) ) {
			
			if ( ENTITY_ASSOC_FLAGS_RANGE( esdr->flags ) ) { /* If contained entities are stored in range */
				
				range = esdr->cntndInst2 - esdr->cntndInst1;

				for ( j = 0; j < range; j++ )

					mchStoreAssocEntInfo( entity, esdr->cntndId1, esdr->cntndInst1 + j, parm, &count );

				if ( esdr->cntndInst2 > 0 ) { /* If second range is also populated */

					range = esdr->cntndInst4 - esdr->cntndInst3;

					for ( j = 0; j < range; j++ )

						mchStoreAssocEntInfo( entity, esdr->cntndId2, esdr->cntndInst2 + j, parm, &count );
				}
			}
			else { /* If contained entities are stored in list */

				mchStoreAssocEntInfo( entity, esdr->cntndId1, esdr->cntndInst1, parm, &count );

				if ( esdr->cntndInst2 > 0 ) /* Second in list exists */

					mchStoreAssocEntInfo( entity, esdr->cntndId2, esdr->cntndInst2, parm, &count );

				if ( esdr->cntndInst3 > 0 ) /* Third in list exists */

					mchStoreAssocEntInfo( entity, esdr->cntndId3, esdr->cntndInst3, parm, &count );

				if ( esdr->cntndInst4 > 0 ) /* Fourth in list exists */

					mchStoreAssocEntInfo( entity, esdr->cntndId4, esdr->cntndInst4, parm, &count );
			}

			if ( 0 == ENTITY_ASSOC_FLAGS_LINK( eassoc->sdr.flags ) )
				break; /* Should be no further assocated entities for this container */
		}
       	}

	if ( parm )
		return 0;
	else
		return count; 
}

static void
mchSdrGetAssocEntInfo(MchData mchData, int parm)
{
MchSys mchSys  = mchData->mchSys;
int  rval, i;
Fru  fru;
Mgmt mgmt;

/* Modify to not duplicate between fru and mgmt */

	for ( i = 0; i < mchSys->fruCount; i++ ) {
		fru = &mchSys->fru[i];

		rval = mchGetAssocEntInfo( mchSys, fru->entity, fru->sdr.addr, fru->sdr.chan, fru->sdr.entityId, fru->sdr.entityInst, parm);

		if ( 0 == parm ) {
			if ( (fru->entityCount = rval) > 0 ) {
				if ( 0 == (fru->entity = calloc( rval, sizeof( EntityRec ) ) ) )
					cantProceed("FATAL ERROR: No memory for FRU entity structure for %s\n", mchData->mchSess->name);
				printf("calloced fru %i\n", (int)(fru->entity));
			}
		}
	}

	for ( i = 0; i < mchSys->mgmtCount; i++ ) {
		mgmt = &mchSys->mgmt[i];
		rval = mchGetAssocEntInfo( mchSys, mgmt->entity, mgmt->sdr.addr, mgmt->sdr.chan, mgmt->sdr.entityId, mgmt->sdr.entityInst, parm);

		if ( 0 == parm ) {
			if ( (mgmt->entityCount = rval) > 0 ) {
				if ( 0 == (mgmt->entity = calloc( rval, sizeof( EntityRec ) ) ) )
					cantProceed("FATAL ERROR: No memory for MGMT entity structure for %s\n", mchData->mchSess->name);
			}
		}
	}
}

static void
mchSdrGetAssocEnt(MchData mchData)
{

	mchSdrGetAssocEntInfo( mchData, 0 );
	mchSdrGetAssocEntInfo( mchData, 1 );

}

/* 
 * Store SDR for one Entity Assocation Record into Entity Assoc data structure
 *
 * Caller must perform locking.
 */
static void
mchSdrEntAssoc(SdrEntAssoc sdr, uint8_t *raw)
{
int n;

	n = SDR_HEADER_LENGTH + raw[SDR_LENGTH_OFFSET];

	sdr->id[0]      = raw[SDR_ID_LSB_OFFSET];
	sdr->id[1]      = raw[SDR_ID_MSB_OFFSET];
	sdr->ver        = raw[SDR_VER_OFFSET];
	sdr->recType    = raw[SDR_REC_TYPE_OFFSET];
	sdr->length     = raw[SDR_LENGTH_OFFSET];

	sdr->cntnrId    = raw[SDR_CNTNR_ENTITY_ID_OFFSET];   
	sdr->cntnrInst  = raw[SDR_CNTNR_ENTITY_INST_OFFSET]; 
	sdr->flags         = raw[SDR_ENTITY_ASSOC_FLAGS_OFFSET];         
	sdr->cntndId1   = raw[SDR_CNTND_ENTITY_ID1_OFFSET];  
	sdr->cntndInst1 = raw[SDR_CNTND_ENTITY_INST1_OFFSET];
	sdr->cntndId2   = raw[SDR_CNTND_ENTITY_ID2_OFFSET];  
	sdr->cntndInst2 = raw[SDR_CNTND_ENTITY_INST2_OFFSET];
	sdr->cntndId3   = raw[SDR_CNTND_ENTITY_ID3_OFFSET];  
	sdr->cntndInst3 = raw[SDR_CNTND_ENTITY_INST3_OFFSET];
	sdr->cntndId4   = raw[SDR_CNTND_ENTITY_ID4_OFFSET];  
	sdr->cntndInst4 = raw[SDR_CNTND_ENTITY_INST4_OFFSET];
}

/* 
 * Store SDR for one Device-relative Entity Assocation Record into Device-relative Entity Assoc data structure
 *
 * Caller must perform locking.
 */
static void
mchSdrDevEntAssoc(SdrDevEntAssoc sdr, uint8_t *raw, uint8_t owner)
{
int n;

	n = SDR_HEADER_LENGTH + raw[SDR_LENGTH_OFFSET];

	sdr->id[0]      = raw[SDR_ID_LSB_OFFSET];
	sdr->id[1]      = raw[SDR_ID_MSB_OFFSET];
	sdr->ver        = raw[SDR_VER_OFFSET];
	sdr->recType    = raw[SDR_REC_TYPE_OFFSET];
	sdr->length     = raw[SDR_LENGTH_OFFSET];

	sdr->cntnrId    = raw[SDR_CNTNR_ENTITY_ID_OFFSET];   
	sdr->cntnrInst  = raw[SDR_CNTNR_ENTITY_INST_OFFSET]; 
	sdr->cntnrAddr     = raw[SDR_DEV_CNTNR_ADDR_OFFSET];    
	sdr->cntnrChan     = raw[SDR_DEV_CNTNR_CHAN_OFFSET];    
	sdr->flags         = raw[SDR_DEV_ENTITY_ASSOC_FLAGS_OFFSET];        
	sdr->cntndAddr1    = raw[SDR_DEV_CNTND_ADDR1_OFFSET];  
	sdr->cntndChan1    = raw[SDR_DEV_CNTND_CHAN1_OFFSET];  
	sdr->cntndId1   = raw[SDR_DEV_CNTND_ENTITY_ID1_OFFSET];  
	sdr->cntndInst1 = raw[SDR_DEV_CNTND_ENTITY_INST1_OFFSET];
	sdr->cntndAddr2    = raw[SDR_DEV_CNTND_ADDR2_OFFSET];  
	sdr->cntndChan2    = raw[SDR_DEV_CNTND_CHAN2_OFFSET];  
	sdr->cntndId2   = raw[SDR_DEV_CNTND_ENTITY_ID2_OFFSET];  
	sdr->cntndInst2 = raw[SDR_DEV_CNTND_ENTITY_INST2_OFFSET];
	sdr->cntndAddr3    = raw[SDR_DEV_CNTND_ADDR3_OFFSET];  
	sdr->cntndChan3    = raw[SDR_DEV_CNTND_CHAN3_OFFSET];  
	sdr->cntndId3   = raw[SDR_DEV_CNTND_ENTITY_ID3_OFFSET];  
	sdr->cntndInst3 = raw[SDR_DEV_CNTND_ENTITY_INST3_OFFSET];
	sdr->cntndAddr4    = raw[SDR_DEV_CNTND_ADDR4_OFFSET];  
	sdr->cntndChan4    = raw[SDR_DEV_CNTND_CHAN4_OFFSET];  
	sdr->cntndId4   = raw[SDR_DEV_CNTND_ENTITY_ID4_OFFSET];  
	sdr->cntndInst4 = raw[SDR_DEV_CNTND_ENTITY_INST4_OFFSET];

printf("found dev-rel entity assord container id 0x%02x inst 0x%02x addr 0x%02x chan %i, flags %i\n", sdr->cntnrId, sdr->cntnrInst, sdr->cntnrAddr, sdr->cntnrChan, sdr->flags );
}

/* Check if this FRU has already been stored in data
 * structure. If so, return -1, else 0
 */
static int
mchSdrFruDuplicate(MchSys mchSys, uint8_t *raw)
{
int i;
SdrFruRec tmp;
FruRec     fru;

	tmp.addr       = raw[SDR_FRU_ADDR_OFFSET];
	tmp.lun        = raw[SDR_FRU_LUN_OFFSET];
	tmp.chan       = raw[SDR_FRU_CHAN_OFFSET];
	tmp.entityId   = raw[SDR_FRU_ENTITY_ID_OFFSET];
	tmp.entityInst = raw[SDR_FRU_ENTITY_INST_OFFSET];

	for ( i = 0; i < mchSys->fruCount; i++ ) {

		fru = mchSys->fru[i];

		if ( (tmp.addr       == fru.sdr.addr)     &&
		     (tmp.lun        == fru.sdr.lun)      &&
		     (tmp.chan       == fru.sdr.chan)     &&
		     (tmp.entityId   == fru.sdr.entityId) &&
		     (tmp.entityInst == fru.sdr.entityInst) ) {

			return -1;
		}
	}
	return 0;
}

/* 
 * Store SDR for one FRU into FRU data structure
 *
 * Caller must perform locking.
 */
static void
mchSdrFruDev(SdrFru sdr, uint8_t *raw)
{
int n, l, i;
	n = SDR_HEADER_LENGTH + raw[SDR_LENGTH_OFFSET];

	sdr->id[0]      = raw[SDR_ID_LSB_OFFSET];
	sdr->id[1]      = raw[SDR_ID_MSB_OFFSET];
	sdr->ver        = raw[SDR_VER_OFFSET];
	sdr->recType    = raw[SDR_REC_TYPE_OFFSET];
	sdr->length     = raw[SDR_LENGTH_OFFSET];
	sdr->addr       = raw[SDR_FRU_ADDR_OFFSET];
	sdr->fruId      = raw[SDR_FRU_ID_OFFSET];
	sdr->lun        = raw[SDR_FRU_LUN_OFFSET];
	sdr->chan       = raw[SDR_FRU_CHAN_OFFSET];
	sdr->devType    = raw[SDR_FRU_TYPE_OFFSET];
	sdr->devMod     = raw[SDR_FRU_TYPE_MOD_OFFSET];
	sdr->entityId   = raw[SDR_FRU_ENTITY_ID_OFFSET];
	sdr->entityInst = raw[SDR_FRU_ENTITY_INST_OFFSET];
	sdr->strLength  = raw[SDR_FRU_STR_LENGTH_OFFSET];

	l = IPMI_DATA_LENGTH( sdr->strLength );
	for ( i = 0; i < l; i++ )
	       	sdr->str[i] = raw[SDR_FRU_STR_OFFSET + i];
       	sdr->str[i+1] = '\0';

}

/* Check if this management controller has already been stored in data
 * structure. If so, return -1, else 0
 */
static int
mchSdrMgmtCtrlDuplicate(MchSys mchSys, uint8_t *raw)
{
int i;
SdrMgmtRec tmp;
MgmtRec    mgmt;

	tmp.addr       = raw[SDR_MGMT_ADDR_OFFSET];
	tmp.chan       = IPMI_CHAN_NUMBER( raw[SDR_MGMT_CHAN_OFFSET] );

	for ( i = 0; i < mchSys->mgmtCount; i++ ) {

		mgmt = mchSys->mgmt[i];

		if ( (tmp.addr == mgmt.sdr.addr)    &&
		     (tmp.chan == mgmt.sdr.chan) ) {
			return -1;
		}
	}
	return 0;
}

/* 
 * Store SDR for one management controller device into data structure
 *
 * Caller must perform locking.
 */
static void
mchSdrMgmtCtrlDev(SdrMgmt sdr, uint8_t *raw)
{
int n, l, i;

	n = SDR_HEADER_LENGTH + raw[SDR_LENGTH_OFFSET];

	sdr->id[0]      = raw[SDR_ID_LSB_OFFSET];
	sdr->id[1]      = raw[SDR_ID_MSB_OFFSET];
	sdr->ver        = raw[SDR_VER_OFFSET];
	sdr->recType    = raw[SDR_REC_TYPE_OFFSET];
	sdr->length     = raw[SDR_LENGTH_OFFSET];
	sdr->addr       = /*IPMI_DEVICE_SLAVE_ADDR( */raw[SDR_MGMT_ADDR_OFFSET] /*)*/;
	sdr->chan       = IPMI_CHAN_NUMBER( raw[SDR_MGMT_CHAN_OFFSET] );
	sdr->pwr        = raw[SDR_MGMT_PWR_OFFSET];
	sdr->cap        = raw[SDR_MGMT_CAP_OFFSET];
	sdr->entityId   = raw[SDR_MGMT_ENTITY_ID_OFFSET];
	sdr->entityInst = raw[SDR_MGMT_ENTITY_INST_OFFSET];
	sdr->strLength  = raw[SDR_MGMT_STR_LENGTH_OFFSET];
			
	l = IPMI_DATA_LENGTH( sdr->strLength );

	for ( i = 0; i < l; i++ )
	       	sdr->str[i] = raw[SDR_MGMT_STR_OFFSET + i];
       	sdr->str[i+1] = '\0';
}

/* Check if this sensor has already been stored in data
 * structure. If so, return -1, else 0
 */
static int
mchSdrSensDuplicate(MchSys mchSys, uint8_t *raw)
{
int i;
SdrFullRec tmp;
SensorRec  sens;

	tmp.owner      = raw[SDR_OWNER_OFFSET];
	tmp.lun        = raw[SDR_LUN_OFFSET];
	tmp.number     = raw[SDR_NUMBER_OFFSET];
	tmp.entityId   = raw[SDR_ENTITY_ID_OFFSET];
	tmp.entityInst = raw[SDR_ENTITY_INST_OFFSET];

	for ( i = 0; i < mchSys->sensCount; i++ ) {

		sens = mchSys->sens[i];

		if ( (tmp.owner      == sens.sdr.owner)    &&
		     (tmp.lun        == sens.sdr.lun)      &&
		     (tmp.number     == sens.sdr.number)   &&
		     (tmp.entityId   == sens.sdr.entityId) &&
		     (tmp.entityInst == sens.sdr.entityInst) ) {

			return -1;
		}
	}
	return 0;
}

/* 
 * Store SDR for one sensor into sensor data structure
 *
 * Caller must perform locking.
 */
static void
mchSdrFullSens(SdrFull sdr, uint8_t *raw, int type)
{
int n, i, l = 0;
int m = 0, b = 0;

	n = SDR_HEADER_LENGTH + raw[SDR_LENGTH_OFFSET];

	sdr->id[0]      = raw[SDR_ID_LSB_OFFSET];
	sdr->id[1]      = raw[SDR_ID_MSB_OFFSET];
	sdr->ver        = raw[SDR_VER_OFFSET];
	sdr->recType    = raw[SDR_REC_TYPE_OFFSET];
	sdr->length     = raw[SDR_LENGTH_OFFSET];
	sdr->owner      = raw[SDR_OWNER_OFFSET];
	sdr->lun        = raw[SDR_LUN_OFFSET];
	sdr->number     = raw[SDR_NUMBER_OFFSET];
	sdr->entityId   = raw[SDR_ENTITY_ID_OFFSET];
	sdr->entityInst = raw[SDR_ENTITY_INST_OFFSET];
	sdr->init       = raw[SDR_INIT_OFFSET];
	sdr->cap        = raw[SDR_CAP_OFFSET];
	sdr->sensType   = raw[SDR_SENS_TYPE_OFFSET];
	sdr->readType   = raw[SDR_READ_TYPE_OFFSET];

	if ( type == SDR_TYPE_COMPACT_SENSOR ) {
		sdr->strLength  = raw[SDR_COMPACT_STR_LENGTH_OFFSET];    

		l = IPMI_DATA_LENGTH( sdr->strLength );
		for ( i = 0; i < l; i++ )
			sdr->str[i] = raw[SDR_COMPACT_STR_OFFSET + i];
		sdr->str[i+1] = '\0';
	}

	/* Full Sensor fields */
	if ( type == SDR_TYPE_FULL_SENSOR ) {

		sdr->units1     = raw[SDR_UNITS1_OFFSET];      
		sdr->units2     = raw[SDR_UNITS2_OFFSET];    
		sdr->units3     = raw[SDR_UNITS3_OFFSET];    
		sdr->linear     = raw[SDR_LINEAR_OFFSET];   
		sdr->M          = raw[SDR_M_OFFSET];   
		sdr->MTol       = raw[SDR_M_TOL_OFFSET];   
		sdr->B          = raw[SDR_B_OFFSET];   
		sdr->BAcc       = raw[SDR_B_ACC_OFFSET];
		sdr->acc        = raw[SDR_ACC_OFFSET];   
		sdr->RexpBexp   = raw[SDR_EXP_OFFSET];   
		sdr->anlgChar   = raw[SDR_ANLG_CHAR_OFFSET];   
		sdr->nominal    = raw[SDR_NOMINAL_OFFSET];   
		sdr->normMax    = raw[SDR_NORM_MAX_OFFSET];   
		sdr->normMin    = raw[SDR_NORM_MIN_OFFSET];   
		sdr->strLength  = raw[SDR_STR_LENGTH_OFFSET];    

		m = SENSOR_CONV_M_B( sdr->M, sdr->MTol );
		b = SENSOR_CONV_M_B( sdr->B, sdr->BAcc );

		sdr->m    = TWOS_COMP_SIGNED_NBIT( m, 10 );
		sdr->b    = TWOS_COMP_SIGNED_NBIT( b, 10 );
		sdr->rexp = TWOS_COMP_SIGNED_NBIT(SENSOR_CONV_REXP( sdr->RexpBexp ), 4 );
		sdr->bexp = TWOS_COMP_SIGNED_NBIT(SENSOR_CONV_BEXP( sdr->RexpBexp ), 4 );


		l = IPMI_DATA_LENGTH( sdr->strLength );
		for ( i = 0; i < l; i++ )
			sdr->str[i] = raw[SDR_STR_OFFSET + i];
		sdr->str[i+1] = '\0';
       	}     

#ifdef DEBUG
printf("mchSdrFullSens: owner 0x%02x lun %i sdr number %i type 0x%02x, m %i, b %i, rexp %i bexp %i, is logical entity %i\n", 
	sdr->owner, sdr->lun, sdr->number, sdr->sensType, sdr->m, sdr->b, sdr->rexp, sdr->bexp, SDR_ENTITY_LOGICAL(sdr->entityInst));
if ( l > 0 )
printf("string %s\n", sdr->str);
#endif

}

/* If sensor ..., return error and indicate 'unavailable' in sensor data structure */
/* This is stricter than it used to be!! Now any non-zero comp code leads to discarding sensor; need to make sure this is okay */
int
mchGetSensorReadingStat(MchData mchData, uint8_t *response, Sensor sens)//, uint8_t number, uint8_t lun, size_t *sensReadMsgLength)
{
uint8_t bits;
uint8_t rval;
size_t  tmp = sens->readMsgLength; /* Initially set to requested msg length, 
					then mchMsgReadSensorWrapper sets it to actual message length */

	rval = mchMsgReadSensorWrapper( mchData, response, sens, &tmp ); //number, lun, sensReadMsgLength );

	/* If error code ... */
	if ( rval ) {
		if ( rval == IPMI_COMP_CODE_REQUESTED_DATA )	
			sens->unavail = 1;
		return -1;
	}
	else
		sens->readMsgLength = tmp;

	bits = response[IPMI_RPLY_IMSG2_SENSOR_ENABLE_BITS_OFFSET];
	if ( IPMI_SENSOR_READING_DISABLED(bits) || IPMI_SENSOR_SCANNING_DISABLED(bits) ) {
		if ( MCH_DBG( mchStat[mchData->mchSess->instance] ) )
			printf("%s mchGetSensorReadingStat: sensor %i reading/state unavailable or scanning disabled. Bits: %02x\n", mchData->mchSess->name, sens->sdr.number, bits);
		return -1;
	}

	return 0;
}

/*
 * Read sensor to save sensor reading response length; it varies
 * by sensor and this prevents read timeouts later.
 * Get sensor thresholds and store them for use by device support.
 * (Assuming thresholds do not change)
 *
 * Caller must perform locking.
 */
static void
mchGetSensorInfo(MchData mchData, Sensor sens)
{
uint8_t  response[MSG_MAX_LENGTH] = { 0 };
size_t   sensReadMsgLength;

	sensReadMsgLength = sens->readMsgLength = IPMI_RPLY_IMSG2_SENSOR_READ_MAX_LENGTH;

	if( !(MCH_ONLN( mchStat[mchData->mchSess->instance] )) ) {
		if ( MCH_DBG( mchStat[mchData->mchSess->instance] ) )
			printf("%s mchGetSensorInfo: MCH offline; aborting\n", mchData->mchSess->name);
		return;
	}

	/* If sensor does not exist, do not read thresholds, etc. */
	mchGetSensorReadingStat( mchData, response, sens);//sens->sdr.number, (sens->sdr.lun & 0x3) , &sensReadMsgLength );/*) {*/	

	if ( sens->unavail )
		return;

	sens->readMsgLength = sensReadMsgLength;

	sens->tmask = 0; /* Set default to no readable thresholds */

	if ( IPMI_SENSOR_THRESH_IS_READABLE( IPMI_SDR_SENSOR_THRESH_ACCESS( sens->sdr.cap ) ) ) {

		if (  mchMsgGetSensorThresholdsWrapper( mchData, response, sens ) ) {
			if ( MCH_DBG( mchStat[mchData->mchSess->instance] ) )
				printf("%s mchGetSensorInfo: mchMsgGetSensorThresholds error, assume no thresholds are readable\n", mchData->mchSess->name);
			return;
		}

		sens->tmask = response[IPMI_RPLY_IMSG2_SENSOR_THRESH_MASK_OFFSET];
		sens->tlnc  = response[IPMI_RPLY_IMSG2_SENSOR_THRESH_LNC_OFFSET];
		sens->tlc   = response[IPMI_RPLY_IMSG2_SENSOR_THRESH_LC_OFFSET];
		sens->tlnr  = response[IPMI_RPLY_IMSG2_SENSOR_THRESH_LNR_OFFSET];
		sens->tunc  = response[IPMI_RPLY_IMSG2_SENSOR_THRESH_UNC_OFFSET];
		sens->tuc   = response[IPMI_RPLY_IMSG2_SENSOR_THRESH_UC_OFFSET];
		sens->tunr  = response[IPMI_RPLY_IMSG2_SENSOR_THRESH_UNR_OFFSET];

		if ( MCH_DBG( mchStat[mchData->mchSess->instance] ) >= MCH_DBG_HIGH )
			printf("sensor %s thresholds tmask 0x%02x, tlnc %i tlc %i tlnr %i tunc %i tuc %i tunr %i\n", 
				sens->sdr.str, sens->tmask, sens->tlnc, sens->tlc, sens->tlnr, sens->tunc, sens->tuc, sens->tunr);
	}
}

/* 'owner' and 'chan' args are address/channel of owner; used only for device-relative entity assocation record
 * 
 */

static void
mchSdrStoreData(MchData mchData, uint8_t *raw, uint8_t type, uint8_t owner, uint8_t chan)
{
MchSys   mchSys  = mchData->mchSys;
Sensor   sens   = 0;
Fru      fru;
Mgmt     mgmt;
EntAssoc entAssoc;
DevEntAssoc devEntAssoc;

	switch ( type ) {

		default:
			break;

		/* Store SDR data; for now we only support Compact/Full Sensor Records, 
		 * FRU Device Locator Records, and Management Controller Device Locator Records 
		 */

		case SDR_TYPE_FULL_SENSOR:
		
			if ( mchSdrSensDuplicate( mchSys, raw ) )
				break;
			sens = &mchSys->sens[mchSys->sensCount];
			mchSdrFullSens( &sens->sdr , raw, type );
			sens->instance = 0; /* Initialize instance to 0 */
			mchGetSensorInfo( mchData, sens );
			sens->cnfg = 0;
			mchSys->sensCount++;

			break;

		case SDR_TYPE_COMPACT_SENSOR:

			if ( mchSdrSensDuplicate( mchSys, raw ) )
				break;
			sens = &mchSys->sens[mchSys->sensCount];
			mchSdrFullSens( &sens->sdr , raw, type );
			sens->instance = 0; /* Initialize instance to 0 */
			mchGetSensorInfo( mchData, sens );
			sens->cnfg = 0;
			mchSys->sensCount++;

			break;

		case SDR_TYPE_FRU_DEV:

			if ( mchSdrFruDuplicate( mchSys, raw ) )
				break;
			fru = &mchSys->fru[mchSys->fruCount];
			mchSdrFruDev( &fru->sdr, raw );
			fru->instance = 0;     /* Initialize instance to 0 */
			mchSys->fruCount++;

			break;

                case SDR_TYPE_MGMT_CTRL_DEV:

                        if ( mchSdrMgmtCtrlDuplicate( mchSys, raw ) )
                                break;
                        mgmt = &mchSys->mgmt[mchSys->mgmtCount];
                        mchSdrMgmtCtrlDev( &mgmt->sdr, raw );
                        mchSys->mgmtCount++;

                        break;

		/* need to check duplicates for entity assoc records ? */
		case SDR_TYPE_ENTITY_ASSOC:
			entAssoc = &mchSys->entAssoc[mchSys->entAssocCount];
			mchSdrEntAssoc( &entAssoc->sdr, raw );
			mchSys->entAssocCount++;

			break;

		case SDR_TYPE_DEV_ENTITY_ASSOC:
			devEntAssoc = &mchSys->devEntAssoc[mchSys->devEntAssocCount];
			devEntAssoc->ownerAddr = owner;
			devEntAssoc->ownerChan = chan;
			mchSdrDevEntAssoc( &devEntAssoc->sdr, raw, owner );

			if ( (devEntAssoc->sdr.cntnrAddr != 0) && (devEntAssoc->sdr.cntnrAddr != owner) ) {
				printf("WARNING: %s Device-relative entity assocation record whose address 0x%02x does not match container address 0x%02x\n",
				    mchData->mchSess->name, devEntAssoc->sdr.cntnrAddr, owner);
			}
			if ( (devEntAssoc->sdr.cntnrChan != 0) && (devEntAssoc->sdr.cntnrChan != chan) ) {
				printf("WARNING: %s Device-relative entity assocation record whose chan 0x%02x does not match container chan 0x%02x\n",
				    mchData->mchSess->name, devEntAssoc->sdr.cntnrChan, chan);
			}
			mchSys->devEntAssocCount++;

			break;
	}
}

/*
 * Read sensor data records. Call ipmiMsgGetSdr twice per record;
 * once to get record length, then to read record. This prevents timeouts,
 * saving much delay. 
 *
 * Caller must perform locking.
 */				  
static int				  
mchSdrGetData(MchData mchData, uint8_t parm, uint8_t addr, uint8_t chan, SdrRep sdrRep)
{
MchSess  mchSess = mchData->mchSess;
MchSys   mchSys  = mchData->mchSys;
uint8_t  response[MSG_MAX_LENGTH] = { 0 };
uint32_t sdrCount;
uint8_t  id[2]  = { 0 };
uint8_t  res[2] = { 0 };
Sensor   sens   = 0;
uint8_t  offset = 0;
uint8_t  type   = 0;
uint8_t *raw    = 0;
int      size; /* SDR record read size (after header) */
uint32_t sdrCount_i  = mchSys->sdrCount; /* Initial SDR count */
int      rval = -1, err = 0, i, remainder = 0;

	if ( mchSdrRepGetInfo( mchData, parm, addr, sdrRep, &sdrCount ) )
		return rval;

	/* Allocate sensor memory for full number of SDRs
	 * This is more than number of sensors, but has benefit 
	 * of being able to store each sensor's ata as we read it
	 */
	if ( !(mchSys->sens = ipmiReallocZerosNewMemory( mchSys->sens, sdrCount_i*sizeof(*sens), (sdrCount_i+sdrCount)*sizeof(*sens) ) ) ) {
		printf("mchSdrGetData: No memory for sensor data for %s\n", mchSess->name);
		goto bail;
	}
	if ( mchMsgReserveSdrRepWrapper( mchData, response, parm, addr ) ) {
		printf("mchSdrGetData: Error reserving SDR repository %s\n", mchSess->name);
		goto bail;
	}

	res[0] = response[IPMI_RPLY_IMSG2_GET_SDR_RES_LSB_OFFSET];
	res[1] = response[IPMI_RPLY_IMSG2_GET_SDR_RES_MSB_OFFSET];

       	for ( i = 0; i < sdrCount; i++) { /* memset raw to zeros on each sdr? */

		if ( !(raw = ipmiReallocZeros( raw, SDR_MAX_LENGTH ) ) ) {
			printf("mchSdrGetData: No memory for raw SDR data for %s\n", mchSess->name);
			goto bail;
		}

		offset = 0;

		size = 5; /* readSize = 5 because 5th byte is remaining record length; 0xFF reads entire record */
       		if ( (mchMsgGetSdrWrapper( mchData, response, id, res, offset, size, parm, addr )) ) {
			i--;
			if ( err++ > 5 ) {
			        printf("mchSdrGetData: too many errors reading SDR for %s\n", mchSess->name);
				continue;
			}
		}
		else {

			size = response[IPMI_RPLY_IMSG2_GET_SDR_DATA_OFFSET + SDR_LENGTH_OFFSET] + SDR_HEADER_LENGTH;
			if ( size > SDR_MAX_LENGTH )
				size = SDR_MAX_LENGTH;
			type = response[IPMI_RPLY_IMSG2_GET_SDR_DATA_OFFSET + SDR_REC_TYPE_OFFSET];

			if ( size > SDR_MAX_READ_SIZE ) {
				remainder = size - SDR_MAX_READ_SIZE;
				size = SDR_MAX_READ_SIZE;
			}

			while ( size > 0 ) {
				if ( mchMsgGetSdrWrapper( mchData, response, id, res, offset, size, parm, addr ) ) {
					i--;
					if ( err++ > 5 ) {
						printf("mchSdrGetData: too many errors reading SDR for %s", mchSess->name);
						continue;
					}
					break;
				}

				else {
					memcpy( raw + + offset, response + IPMI_RPLY_IMSG2_GET_SDR_DATA_OFFSET, size );
					offset += size;
				}

				if ( remainder > SDR_MAX_READ_SIZE ) {
					remainder = remainder - SDR_MAX_READ_SIZE;
					size = SDR_MAX_READ_SIZE;
				}
				else {
					size = remainder;
					remainder = 0;
				}
			}

			mchSdrStoreData( mchData, raw, type, addr, chan );
			id[0]  = response[IPMI_RPLY_IMSG2_GET_SDR_NEXT_ID_LSB_OFFSET];
			id[1]  = response[IPMI_RPLY_IMSG2_GET_SDR_NEXT_ID_MSB_OFFSET];

			if ( arrayToUint16( id ) == SDR_ID_LAST_SENSOR ) // last record in SDR
				break;
		}
       	}

	mchSys->sdrCount += sdrCount;
	rval = 0;

bail:

	if ( raw )
		free( raw );
	return rval;
}

static int				  
mchSdrGetDataAll(MchData mchData)
{
MchSess  mchSess = mchData->mchSess;
MchSys   mchSys  = mchData->mchSys;
int      i, j, rval = 0; /* think about how to handle rval */
Mgmt     mgmt;
uint8_t  addr = 0;
int      inst = mchSess->instance;
Sensor   sens   = 0;
Fru      fru;
uint8_t  response[MSG_MAX_LENGTH] = { 0 };
EntAssoc entAssoc;
DevEntAssoc devEntAssoc;
uint8_t  chan = 0;

	/* First get BMC SDR Rep info */
	if ( mchSdrGetData( mchData, IPMI_SDRREP_PARM_GET_SDR, IPMI_MSG_ADDR_BMC, chan, &mchSys->sdrRep ) ) {
		printf("mchSdrGetData: Error in reading primary SDR\n");
		return rval;
	}
	for ( i = 0; i < mchSys->mgmtCount; i++ ) {

		mgmt = &mchSys->mgmt[i];
		addr = mgmt->sdr.addr;
		chan = mgmt->sdr.chan;

		/* Skip primary BMC; already queried */
		if ( addr == IPMI_MSG_ADDR_BMC )
			continue;

		if ( mchMsgGetDeviceIdWrapper( mchData, response, addr ) ) {
			printf("mchSdrGetDataAll: Error from Get Device ID command to mgmt controller at addr 0x%02x\n", addr);
			continue;
		}

		/* If supports SDR... */
		if ( (IPMI_DEV_CAP_SDRREP( response[IPMI_RPLY_IMSG2_GET_DEVICE_ID_SUPPORT_OFFSET] ) ) ) { 
			if ( mchSdrGetData( mchData, IPMI_SDRREP_PARM_GET_SDR, addr, chan, &mgmt->sdrRep ) ) {
				printf("mchSdrGetDataAll: Error in reading mgmt %i SDR\n", i);
			}
		}
		/* Else if supports device SDR... */
		else if ( IPMI_DEVICE_PROVIDES_DEVICE_SDR(response[IPMI_RPLY_IMSG2_GET_DEVICE_ID_DEVICE_VERS_OFFSET]) ) { 

			if ( mchSdrGetData( mchData, IPMI_SDRREP_PARM_GET_DEV_SDR, addr, chan, &mgmt->sdrRep ) ) {
				printf("mchSdrGetDataAll: Error in reading mgmt %i SDR\n", i);
			}
			else
				printf("mchSdrGetDataAll: Successfully read mgmt %i SDR\n", i);
		}

		/* If provides FRU info, create a FRU instance for it in our data structure */
		if ( (IPMI_DEV_CAP_FRU_INV( response[IPMI_RPLY_IMSG2_GET_DEVICE_ID_SUPPORT_OFFSET] ) ) ) { 

			fru = &mchSys->fru[mchSys->fruCount];
			mchSys->fruCount++;
			fru->sdr.addr  = mgmt->sdr.addr;
			fru->sdr.chan  = mgmt->sdr.chan;
			fru->sdr.fruId = 0; /* Arbitrarily set to 0 */
			fru->sdr.entityId   = mgmt->sdr.entityId;
			fru->sdr.entityInst = mgmt->sdr.entityInst;
			fru->sdr.recType    = mgmt->sdr.recType; /* Because recType is used as validity check in various places */			
		}
	}

	/* Find associated entitites */
	mchSdrGetAssocEnt( mchData );

	if ( MCH_DBG( mchStat[inst] ) >= MCH_DBG_MED ) {
		printf("%s mchSdrGetDataAll Summary:\n", mchSess->name);
		for ( i = 0; i < mchSys->sensCount; i++ ) {
			sens = &mchSys->sens[i];
			printf("SDR %i, %s entity ID 0x%02x, entity inst 0x%02x, sensor number %i, sens type 0x%02x, "
				"owner 0x%02x, LUN %i, RexpBexp %i, M %i, MTol %i, B %i, BAcc %i\n", 
				sens->sdr.number, sens->sdr.str, sens->sdr.entityId, sens->sdr.entityInst, 
				sens->sdr.number, sens->sdr.sensType, sens->sdr.owner, sens->sdr.lun, 
				sens->sdr.RexpBexp, sens->sdr.M, sens->sdr.MTol, sens->sdr.B, sens->sdr.BAcc);
		}
		for ( i = 0; i < mchSys->fruCount; i++ ) {
			fru = &mchSys->fru[i];
			if ( fru->sdr.recType )
				printf("FRU %i, recType 0x%02x, entity ID 0x%02x, entity inst 0x%02x,, addr 0x%02x, chan %i, FRU id %i, %s, entity count %i\n", 
				i, fru->sdr.recType, fru->sdr.entityId, fru->sdr.entityInst, fru->sdr.addr, fru->sdr.chan, fru->sdr.fruId, fru->sdr.str, fru->entityCount);
			for ( j = 0; j < fru->entityCount; j++ ) {
				printf("     Associated entity addr 0x%02x chan %i entityId 0x%02x entityInst 0x%02x\n", 
				fru->entity[j].addr, fru->entity[j].chan, fru->entity[j].entityId, fru->entity[j].entityInst); 
			}
		}
		for ( i = 0; i < mchSys->mgmtCount; i++ ) {
			mgmt = &mchSys->mgmt[i];
       			printf("Mgmt Ctrl %i, FRU entity ID 0x%02x, entity inst 0x%02x, %s, addr 0x%02x, chan %i, cap 0x%02x entity count %i\n", 
				i, mgmt->sdr.entityId, mgmt->sdr.entityInst, mgmt->sdr.str, mgmt->sdr.addr, mgmt->sdr.chan, mgmt->sdr.cap, mgmt->entityCount);
			for ( j = 0; j < mgmt->entityCount; j++ ) {
				printf("     Associated entity addr 0x%02x chan %i entityId 0x%02x entityInst 0x%02x\n", 
				mgmt->entity[j].addr, mgmt->entity[j].chan, mgmt->entity[j].entityId, mgmt->entity[j].entityInst); 
			}
		}

		for ( i = 0; i < mchSys->devEntAssocCount; i++ ) {
			devEntAssoc = &mchSys->devEntAssoc[i];
       			printf("Dev Ent Assoc %i, owner addr 0x%02x container ent id 0x%02x inst 0x%02x addr 0x%02x chan %i, flags %i\n"
				"     contained 1 addr 0x%02x chan %i entity id 0x%02x inst 0x%02x\n" 
				"     contained 2 addr 0x%02x chan %i entity id 0x%02x inst 0x%02x\n" 
				"     contained 3 addr 0x%02x chan %i entity id 0x%02x inst 0x%02x\n" 
				"     contained 4 addr 0x%02x chan %i entity id 0x%02x inst 0x%02x\n", 
				i, devEntAssoc->ownerAddr, devEntAssoc->sdr.cntnrId, devEntAssoc->sdr.cntnrInst, devEntAssoc->sdr.cntnrAddr, devEntAssoc->sdr.cntnrChan, devEntAssoc->sdr.flags, 
				devEntAssoc->sdr.cntndAddr1, devEntAssoc->sdr.cntndChan1, devEntAssoc->sdr.cntndId1, devEntAssoc->sdr.cntndInst1,
				devEntAssoc->sdr.cntndAddr2, devEntAssoc->sdr.cntndChan2, devEntAssoc->sdr.cntndId2, devEntAssoc->sdr.cntndInst2,
				devEntAssoc->sdr.cntndAddr3, devEntAssoc->sdr.cntndChan3, devEntAssoc->sdr.cntndId3, devEntAssoc->sdr.cntndInst3,
				devEntAssoc->sdr.cntndAddr4, devEntAssoc->sdr.cntndChan4, devEntAssoc->sdr.cntndId4, devEntAssoc->sdr.cntndInst4);
		}
		for ( i = 0; i < mchSys->entAssocCount; i++ ) {
			entAssoc = &mchSys->entAssoc[i];
       			printf("Ent Assoc %i, container ent id 0x%02x inst 0x%02x, flags %i\n"
				"     contained entity 1 id 0x%02x inst 0x%02x\n" 
				"     contained entity 2 id 0x%02x inst 0x%02x\n" 
				"     contained entity 3 id 0x%02x inst 0x%02x\n" 
				"     contained entity 4 id 0x%02x inst 0x%02x\n",
				i, entAssoc->sdr.cntnrId, entAssoc->sdr.cntnrInst, entAssoc->sdr.flags, 
				entAssoc->sdr.cntndId1, entAssoc->sdr.cntndInst1,
				entAssoc->sdr.cntndId2, entAssoc->sdr.cntndInst2,
				entAssoc->sdr.cntndId3, entAssoc->sdr.cntndInst3,
				entAssoc->sdr.cntndId4, entAssoc->sdr.cntndInst4);
		}
	}

	return rval;
}

/* 
 *  Periodically ping MCH. This runs in its own thread.
 *  If MCH online/offline status changes, update global
 *  variable and process status record. Less frequently set
 *  flag to check that MCH data structs match real hardware.
 *
 *  These messages are outside of a session and we don't
 *  modify our shared structure, so there is no need to lock
 *  during the message write. We call ipmiMsgWriteRead directly, 
 *  instead of using the helper routine which tries to recover a 
 *  disconnected session.
 */

static void
mchPing(void *arg)
{
MchDev  mch     = arg;
MchData mchData = mch->udata;
MchSess mchSess = mchData->mchSess;
uint8_t message[MSG_MAX_LENGTH]  = { 0 };
uint8_t response[MSG_MAX_LENGTH] = { 0 };
size_t  responseSize, responseLen; /* expected, actual */
int     cos; /* change of state */
int     inst = mchSess->instance, i = 0,j  = 0;

	memcpy( message, RMCP_HEADER, sizeof( RMCP_HEADER ) );

	message[RMCP_MSG_CLASS_OFFSET] = RMCP_MSG_CLASS_ASF;

	memcpy( message + ASF_MSG_OFFSET, ASF_MSG, sizeof( ASF_MSG ) );

	while (1) {

		cos = 0;
		responseSize = RMCP_MSG_HEADER_LENGTH + ASF_MSG_HEADER_LENGTH + ASF_RPLY_PONG_PAYLOAD_LENGTH;

		ipmiMsgWriteRead( mchSess->name, message, sizeof( RMCP_HEADER ) + sizeof( ASF_MSG ), 
			response, &responseSize, RPLY_TIMEOUT_DEFAULT, &responseLen );

		if ( responseLen == 0 ) {

			if ( MCH_ONLN( mchStat[inst] ) ) {
				if ( MCH_DBG( mchStat[inst] ) )
					printf("%s mchPing now offline\n", mchSess->name);
				mchStatSet( inst, MCH_MASK_ONLN, 0 );
				cos = 1;
			}
		}
		else {
			if ( !MCH_ONLN( mchStat[inst] ) ) {
				if ( MCH_DBG( mchStat[inst] ) )
					printf("%s mchPing now online\n", mchSess->name);
				mchStatSet( inst, MCH_MASK_ONLN, MCH_MASK_ONLN );
				cos = 1;
			}
			/* Every 60 seconds (while mch online) , set flag to check if system configuration has changed */
			if ( i > 60/PING_PERIOD ) {
				mchStatSet( inst, MCH_MASK_CNFG_CHK, MCH_MASK_CNFG_CHK );
				i = 0;
			}
			/* Every 20 seconds (while mch online), scan sensor records */
			if ( j > 20/PING_PERIOD ) {
				if ( drvSensorScan[inst] ) 
					scanIoRequest( drvSensorScan[inst] );
				j = 0;
			}
			i++;
			j++;
		}

		if ( cos ) {
       			if ( drvMchStatScan )
       				scanIoRequest( drvMchStatScan );
		}

		epicsThreadSleep( PING_PERIOD );
	}
}

/* Check if MCH configuration has changed or if
 * MCH is online and we have not read its configuration.
 * If either, get MCH data and store in our data structs
 */
int
mchCnfgChk(MchData mchData) {
int    inst   = mchData->mchSess->instance;

	mchStatSet( inst, MCH_MASK_CNFG_CHK, 0 );

	if ( MCH_INIT_NOT_DONE( mchStat[inst] ) )
		return mchCnfg( mchData );

	else if ( MCH_INIT_DONE( mchStat[inst] ) ) { 
		if ( mchSdrRepTsDiff( mchData ) )
			return mchCnfg( mchData );
	}

	return 0;
}

/* Set all elements of 3D array to same integer value */
static void
set3DArrayVals(int a, int b, int c, int arr[a][b][c], int val) {
int i, j, k;

	for ( i = 0; i < a; i++ ) {
		for ( j = 0; j < b; j++ ) {
			for ( k = 0; k < c; k++ ) {
				arr[i][j][k] = val;
			}
		}
	}
}

/* Set all elements of 1D array to same integer value */
static void
set1DArrayVals(int a, int arr[a], int val) {
int i;

	for ( i = 0; i < a; i++ ) {
		arr[i] = val;
	}
}

/* 
 * Modify MCH status mask. If record-related changes
 * were made, scan associated EPICS records
 */
void
mchStatSet(int inst, uint32_t clear, uint32_t set) {

	epicsMutexLock(   mchStatMtx[inst] );
	mchStat[inst] &= ~clear;
	mchStat[inst] |=  set;
	epicsMutexUnlock( mchStatMtx[inst] );

	if ( clear == MCH_MASK_INIT) {
printf("scan init pv clear %i set %i\n", clear, set);
		if ( drvMchInitScan ) {
printf("before scaniorquest of drvMchInitScan\n");
			scanIoRequest( drvMchInitScan );
printf("after scaniorquest of drvMchInitScan\n");
}
	}

	if ( (clear == MCH_MASK_ONLN) || (set == MCH_MASK_INIT_DONE) ) {
		if ( drvMchStatScan ) {
printf("drvMchStatScan\n");
			scanIoRequest( drvMchStatScan );
}
	}
}

static void
mchSetFeatures(MchData mchData)
{
MchSess  mchSess  = mchData->mchSess;
IpmiSess ipmiSess = mchData->ipmiSess;

	if ( mchSess->type == MCH_TYPE_VT )
		mchSess->timeout = ipmiSess->timeout = RPLY_TIMEOUT_SENDMSG_RPLY;
	else
		mchSess->timeout = ipmiSess->timeout = RPLY_TIMEOUT_DEFAULT;

	if ( mchSess->type == MCH_TYPE_VT )
		ipmiSess->features |= MCH_FEAT_SENDMSG_RPLY;		
}	

static void
*mchSetIdentity(MchSess mchSess, char *vendor, uint8_t vers, int type, char *cbname)
{
void *cb;

	printf("Identified %s to be %s. IPMI version %i.%i\n", mchSess->name, vendor, IPMI_VER_MSD( vers ), IPMI_VER_LSD( vers ));
	mchSess->type = type;
	if ( !(cb = registryFind( (void *)mchCbRegistryId, cbname )) ) {
		printf("mchIdentify: ERROR for %s: failed to find callbacks %s\n", mchSess->name, cbname);
		return 0;
	}
	return cb;
}

/* 
 * Use Manufacturer ID (from Get Device ID command)
 * to determine MCH type
 */
static int
mchIdentify(MchData mchData)
{
MchSess  mchSess = mchData->mchSess;
int      i;
uint8_t  response[MSG_MAX_LENGTH] = { 0 };
uint8_t  tmp[4] = { 0 }, vers;
uint32_t mf;
void    *mchcb = 0;
char    *vendor[25];

	if ( mchMsgGetDeviceIdWrapper( mchData, response, IPMI_MSG_ADDR_BMC ) ) {
		printf("mchIdentify: Error from Get Device ID command\n");
		mchMsgCloseSess( mchSess, mchData->ipmiSess, response );
		return -1;
	}

	/* Extract Manufacturer ID */
	for ( i = 0; i < IPMI_RPLY_MANUF_ID_LENGTH ; i++)
		tmp[i] = response[IPMI_RPLY_IMSG2_GET_DEVICE_ID_MANUF_ID_OFFSET + i];

       	vers = response[IPMI_RPLY_IMSG2_GET_DEVICE_ID_IPMI_VERS_OFFSET];

	mf  = arrayToUint32( tmp  );
	mf  = IPMI_MANUF_ID( mf  );

	switch ( mf ) {

		default: 
			printf("mchIdentify: Unknown type of MCH, failed to match manuf ID 0x%08x\n", mf);
			mchSess->type = MCH_TYPE_UNKNOWN;
			return -1;

		case MCH_MANUF_ID_NAT:
			if ( 0 == (mchcb = mchSetIdentity( mchSess, "N.A.T.", vers, MCH_TYPE_NAT, "drvMchMicrotcaNatCb")) )
				return -1;
			break;

		case MCH_MANUF_ID_VT:
			if ( 0 == (mchcb = mchSetIdentity( mchSess, "Vadatech", vers, MCH_TYPE_VT, "drvMchMicrotcaVtCb")) )
				return -1;
			break;
/*  Not yet supported     
		case MCH_MANUF_ID_DELL:
			mchSess->type = MCH_TYPE_DELL;
			printf("Identified %s to be Dell. IPMI version %i.%i\n", mchSess->name, IPMI_VER_MSD( vers ), IPMI_VER_LSD( vers ));
			break;
*/
		case MCH_MANUF_ID_SUPERMICRO:
			if ( 0 == (mchcb = mchSetIdentity( mchSess, "Supermicro", vers, MCH_TYPE_SUPERMICRO, "drvMchSupermicroCb")) )
				return -1;
			break;

		case MCH_MANUF_ID_PENTAIR:
			if ( 0 == (mchcb = mchSetIdentity( mchSess, "Pentair", vers, MCH_TYPE_PENTAIR, "drvMchAtcaCb")) )
				return -1;
			break;

		case MCH_MANUF_ID_ARTESYN:
			if ( 0 == (mchcb = mchSetIdentity( mchSess, "Artesyn", vers, MCH_TYPE_ARTESYN, "drvMchAtcaCb")) )
				return -1;
			break;
	}

	/* Callbacks and init for comm with knob controller */
	mchData->mchSys->mchcb = (MchCbRec *)mchcb;

	mchSetFeatures( mchData );
        return 0;
}

static void
mchSensorFruGetInstance(MchData mchData)
{
MchSys  mchSys= mchData->mchSys;
int     s = mchSys->sensCount;
uint8_t sensTypeInst[MAX_FRU_MGMT][MAX_SENSOR_TYPE] = { { 0 } };    /* Counts of sensor type per ID instance */
int     j, index;
Fru     fru  = 0;
Mgmt    mgmt = 0;
Sensor  sens;

	/* Find FRU or management controller associated with this sensor, assign sensor an instance based on type */
	for ( j = 0; j < s; j++ ) {

		sens = &mchSys->sens[j];

		if ( -1 == (index = sens->fruIndex) )
			continue;

		/* Skip FRU 0 for MicroTCA because it is reserved logical entity with same 
		 * entityId and entityInst as MCH 1. This should be in callbacks
		 */
		if ( (sens->fruId == 0) && MCH_IS_MICROTCA( mchData->mchSess->type ) )
			continue;

		if ( index < MAX_FRU ) {

			fru = &mchSys->fru[index];

			/* If a real entity */
			if ( fru->sdr.recType ) {

				sens->instance = ++sensTypeInst[index][sens->sdr.sensType];
				if ( sens->instance > MAX_SENS_INST ) {
					printf("WARNING: FRU %i sensor type 0x%02x instance %i exceeds allowed instance %i\n",
					    index, sens->sdr.sensType, sens->instance, MAX_SENS_INST);
					continue;
				}

				if ( !sens->unavail )
					mchSys->sensLkup[index][sens->sdr.sensType][sens->instance] = j;
			}
		}
	}

#ifdef DEBUG
int i;
printf("mchSensorFruGetInstance:\n");
for ( i = 0; i < s; i++ ) {
	sens = &mchSys->sens[i];
	if ( sens->fruIndex != -1 ) {
		fru  = &mchSys->fru[sens->fruIndex];
		printf("FRU sensor %s %i, type 0x%02x, inst %i, FRU addr 0x%02x, entId 0x%02x, entInst 0x%02x, "
			"sens owner 0x%02x, entId 0x%02x  entInst 0x%02x recType %i fruId %i fruLkup %i fruIndex %i unavail %i\n",
			sens->sdr.str, sens->sdr.number, sens->sdr.sensType, sens->instance, fru->sdr.addr, 
			fru->sdr.entityId, fru->sdr.entityInst, sens->sdr.owner, sens->sdr.entityId, 
			sens->sdr.entityInst, sens->sdr.recType, sens->fruId, mchGetFruLkup( mchData, sens->fruIndex ), 
			sens->fruIndex, sens->unavail);
	}
	else if ( sens->mgmtIndex != -1 ) {
		mgmt  = &mchSys->mgmt[sens->mgmtIndex];
		printf("Mgmt sensor %s %i, type 0x%02x, inst %i, MGMT addr 0x%02x, entId 0x%02x, entInst 0x%02x, "
			"sens owner 0x%02x, entId 0x%02x  entInst 0x%02x recType %i fruLkup %i unavail %i\n",
			sens->sdr.str, sens->sdr.number, sens->sdr.sensType, sens->instance, mgmt->sdr.addr, 
			mgmt->sdr.entityId, mgmt->sdr.entityInst, sens->sdr.owner, sens->sdr.entityId, 
			sens->sdr.entityInst, sens->sdr.recType, mchGetFruLkup( mchData, sens->mgmtIndex ), sens->unavail);
	}
	else
		printf("Sensor %s %i, type 0x%02x, inst %i, entId 0x%02x  entInst 0x%02x recType %i, "
			"no corresponding FRU or MGTM unavail %i\n",
			sens->sdr.str, sens->sdr.number, sens->sdr.sensType, sens->instance, sens->sdr.entityId, 
			sens->sdr.entityInst, sens->sdr.recType, sens->unavail);
}
#endif

}

static void
mchCnfgReset(MchData mchData) {
MchSys mchSys = mchData->mchSys;

	if ( !(mchSys->fru = ipmiReallocZeros( mchSys->fru , MAX_FRU*sizeof(FruRec) )) )
		cantProceed("FATAL ERROR: No memory for FRU data for %s\n", mchData->mchSess->name);

	if ( !(mchSys->mgmt = ipmiReallocZeros( mchSys->mgmt , MAX_MGMT*sizeof(MgmtRec) )) )
		cantProceed("FATAL ERROR: No memory for Management Controller data for %s\n", mchData->mchSess->name);

	/* move this to reset values routine, add fruid, fuindex, etc. */
	set3DArrayVals( MAX_FRU_MGMT, MAX_SENSOR_TYPE, MAX_SENS_INST, mchSys->sensLkup, -1 );
	set1DArrayVals( MAX_FRU_MGMT, mchSys->fruLkup, -1 );
}

/* Get info about MCH, shelf, FRUs, sensors
 * Caller must perform locking
 */
static int
mchCnfg(MchData mchData) {
MchSess mchSess = mchData->mchSess;
MchSys  mchSys  = mchData->mchSys;
int inst = mchSess->instance;
int i;

	mchStatSet( inst, MCH_MASK_INIT, MCH_MASK_INIT_IN_PROGRESS );

	mchCnfgReset( mchData );

	mchSeqInit( mchData->ipmiSess );

	/* Turn on debug messages during initial messages with device */
//	mchStatSet( inst, MCH_MASK_DBG, MCH_DBG_SET(MCH_DBG_LOW) );
	mchStatSet( inst, MCH_MASK_DBG, MCH_DBG_SET(MCH_DBG_MED) );

	/* Initiate communication session with MCH */
	if ( mchCommStart( mchSess, mchData->ipmiSess ) ) {
		printf("Error initiating session with %s; cannot complete initialization\n",mchSess->name);
		goto bail;
	}

        /* Determine MCH type */
        if ( mchIdentify( mchData ) ) {
	       	printf("Failed to identify %s MCH type; cannot complete initialization\n",mchSess->name);	
		goto bail;
	}

	/* Turn off debug messages after initial messages with device */
//	mchStatSet( inst, MCH_MASK_DBG, MCH_DBG_SET(MCH_DBG_OFF) );

	/* Intialize sensor and device counts to 0 */
	mchSys->sdrCount = mchSys->sensCount = mchSys->fruCount = mchSys->mgmtCount = 0;

       	/* Get SDR data */
       	if ( mchSdrGetDataAll( mchData ) ) {
       		printf("Failed to read %s SDR; cannot complete initialization\n",mchSess->name);
		goto bail;
       	}

	/* Get FRU data; errors are not fatal, but could cause missing FRU data */
	mchFruGetDataAll( mchData );
 
		/* Comment this out for now; it always happens at the end of initialization and it is confusing
		printf("Warning: errors getting %s FRU data; some data may be missing\n",mchSess->name);
		*/

	/* Get Sensor/FRU association */
	for ( i = 0; i < mchSys->sensCount; i++ )
		mchSensorGetFru( mchData, i );

	mchSensorFruGetInstance( mchData );

	mchStatSet( inst, MCH_MASK_INIT, MCH_MASK_INIT_DONE );

	if ( drvMchFruScan )
		scanIoRequest( drvMchFruScan );

	mchStatSet( inst, MCH_MASK_DBG, MCH_DBG_SET(MCH_DBG_OFF) );

	return 0;

bail:
	mchStatSet( inst, MCH_MASK_INIT, MCH_MASK_INIT_NOT_DONE );
	return -1;
}


// !! need to make sure initial values do not lead to records sending messages i.e. assume not initialized if cannot establish session

/* 
 * MCH initialization. Called before iocInit.
 *
 * Initial release requires MCH/shelf to be on-line
 * and populated and creates new epics records script.  
 * Later release should derive info from previously
 * created script.
 */
static void
mchInit(const char *name)
{
MchDev   mch     = 0; /* Device support data structure */
MchData  mchData = 0; /* MCH-specific info */
MchSess  mchSess = 0;
IpmiSess ipmiSess = 0;
MchSys   mchSys  = 0;
char     taskName[MAX_NAME_LENGTH+10];
int      inst;

	/* Allocate memory for MCH data structures */
	if ( ! (mchData = calloc( 1, sizeof( *mchData ))) )
		cantProceed("FATAL ERROR: No memory for MchData structure for %s\n", name);

	if ( ! (mchSess = calloc( 1, sizeof( *mchSess ))) )
		cantProceed("FATAL ERROR: No memory for MchSess structure for %s\n", name);

	if ( ! (ipmiSess = calloc( 1, sizeof( *ipmiSess ))) )
		cantProceed("FATAL ERROR: No memory for IpmiSess structure for %s\n", name);

	if ( ! (mchSys = calloc( 1, sizeof( *mchSys ))) )
		cantProceed("FATAL ERROR: No memory for MchSys structure for %s\n", name);

	ipmiSess->wrf = (IpmiWriteReadHelper)mchMsgWriteReadHelper;

	inst = mchSess->instance = mchCounter++;

	mchStatMtx[inst] = epicsMutexMustCreate(); /* Used for global mchStat mask */

       	/* Allocate memory for MCH device support structure */
       	if ( ! (mch = calloc( 1, sizeof( *mch ) )) )
       		cantProceed("FATAL ERROR: No memory for MchDev structure\n");

       	if ( ! (mch = devMchRegister( name )) )
       		printf("FATAL ERROR: Unable to register MCH %s with device support\n", name);

	mchData->ipmiSess = ipmiSess;
	mchData->mchSess = mchSess;
	mchData->mchSys  = mchSys;

       	strncpy( mchSess->name, mch->name, MAX_NAME_LENGTH );
       	strncpy( mchSys->name,  mch->name, MAX_NAME_LENGTH ); // okay to remove this and from drvMch.h?
       	mch->udata = mchData;

       	mchSess->timeout = ipmiSess->timeout = RPLY_TIMEOUT_SENDMSG_RPLY; /* Default, until determine type */
	mchSess->session = 1;   /* Default: enable session with MCH */

	/* For sensor record scanning */
	scanIoInit( &drvSensorScan[inst] );

	/* Start task to periodically ping MCH */
	sprintf( taskName, "%s-PING", mch->name ); 
	mchSess->pingThreadId = epicsThreadMustCreate( taskName, epicsThreadPriorityMedium, epicsThreadGetStackSize(epicsThreadStackMedium), mchPing, mch );

	/* Wait for updated status from ping thread */ 
	epicsThreadSleep( 2 ); 

	if (  MCH_ONLN( mchStat[inst] ) ) {
		epicsMutexLock( mch->mutex );
		mchCnfg( mchData );
		epicsMutexUnlock( mch->mutex );
	}
	else {
		mchCnfgReset( mchData ); /* Initialize some data structs and values */
		printf("No response from %s; cannot complete initialization\n",mch->name);
	}
}

static long
drvMchReport(int level)
{
	printf("IPMI communication driver support\n");
	return 0;
}

static long
drvMchInit(void)
{
	return 0;
}

static struct {
	long number;
	DRVSUPFUN report;
	DRVSUPFUN init;
} drvMch={
	2,
	drvMchReport,
	drvMchInit
};

epicsExportAddress(drvet,drvMch);


/* 
 * IOC shell command registration
 */
static const iocshArg mchInitArg0        = { "port name",iocshArgString};
static const iocshArg *mchInitArgs[1]    = { &mchInitArg0 };
static const iocshFuncDef mchInitFuncDef = { "mchInit", 1, mchInitArgs };

static void 
mchInitCallFunc(const iocshArgBuf *args)
{
	mchInit(args[0].sval);
}

static void
drvMchRegisterCommands(void)
{
	static int firstTime = 1;
	if ( firstTime ) {
		iocshRegister(&mchInitFuncDef, mchInitCallFunc);
		firstTime = 0;
	}
}

epicsExportRegistrar(drvMchRegisterCommands);
