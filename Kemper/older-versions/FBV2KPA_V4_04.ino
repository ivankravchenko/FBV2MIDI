/*!
*  @file       FBV2KPA_V4_04.ino (V4 for Kemper OS4.x)
*  Project     Control the Kemper Profiling Amplifier with a Line6 FBV Longboard
*  @brief      Control KPA with FBV
*  @version    4.4
*  @author     Joachim Wrba
*  @date       2016.10.23
*  @license    GPL v3.0
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
* New in 4.4:
*  Scanning Rig Tag "Comment" for pedal assignment
*    Tags in Comment:
*      P<pdlNum><request>=<value>
*         <pdlNum>:
*           1 or 2
*         <request>:
*           A = activeControl
*           I = inactiveControl (activated via switch)
*               <value>:
*                 _ =  none
*                 W = Wah
*                 V = Vol
*                 P = Pitch
*                 M = Morph
*                 G = Gain
*           P = Pedal Position of the active contoller
*               <value>:
*                 H = Heel  (1 not 0)
*                 T = Toe   (127)
*                 1 - 9 = Volume Steps of 12
*   If Volume is not within the active controllers, a volume of 127 is sent.
*
*
*
*   Pedal LEDs show Controller Type
*   |       |Green  |Red    |
*   +++++++++++++++++++++++++
*   | none  | off   | off   |
*   | Wah   | off   | flash |
*   | Gain  | off   | on    |
*   | Vol   | on    | off   |
*   | Pitch | flash | off   |
*   | Morph | flash | flash |
*
* New in 4.3:
*   Bug fixed whwn switching the same slot in the next performance
* New in 4.2:
*   Scroll through performaces when holding bank up/down
*/


//=========================================================================
//============= Serial Ports of the Arduino Mega ==========================
#define SERIAL_FBV Serial1
#define SERIAL_KPA Serial3
//=========================================================================

//============= Debug switches ============================================
//=========================================================================
#define DEBUG_kpa_handle_param_single 0
#define DEBUG_kpa_handle_param_string 0
#define DEBUG_onKpaSysEx 0
#define DEBUG_fDisplayTuner 0
//=========================================================================

//=========================================================================
//==== Changeable Values ==================================================
#define FBV_HOLD_TIME 1500
#define FLASH_TIME 1000
#define FLASH_TIME_FAST 500

// wait for requesting, as KPA sends some information on program change
// so not all requests need to be sent
#define PAUSE_REQUESTS 20
#define PAUSE_REQUESTS_LONG 200
//=========================================================================

#include "Line6Fbv.h"
#include "KPA_defines.h"
#include <MIDI.h>

// positions in the array
#define FX_SLOT_POS_A  0
#define FX_SLOT_POS_B  1
#define FX_SLOT_POS_C  2
#define FX_SLOT_POS_D  3
#define FX_SLOT_POS_X  4
#define FX_SLOT_POS_MOD  5
#define FX_SLOT_POS_DLY  6
#define FX_SLOT_POS_REV  7

//
#define CC_BANK_LSB  0x20


MIDI_CREATE_INSTANCE(HardwareSerial, SERIAL_KPA, mMidiKpa);


Line6Fbv mFbv = Line6Fbv();

int mBPM = 0;

byte mMidiBank = 0;
byte mMidiPgm = 0;

bool mLooperMode = false;

bool mSendInitialRequests = false;
bool mSendNextRequest = true;
unsigned long mReqestLastSent = 0;
unsigned long mPauseRequestsUntil = 0;

char mPerformanceSlotNames[5][17] = { { 0x00 } };
char mRigName[17] = { 0x00 };
char mRigComment[100] = { 0x00 };


struct kpa_state_ {
	int tune;                /* Holds the current tune value */
	uint8_t noteNum;             /* Holds the current tuner note */
	uint8_t octave;
	uint8_t mode;						 // Holds the mode KPA is running (TUNER, BROWSE, PERFORMANCE)
};


struct kpa_state_ kpa_state = {
	0,
	0,
	0,
	KPA_MODE_PERFORM,
};

struct fbv_pedal{
	byte ctlNumOff;
	byte ctlNumOn;
	byte ctlNum;
	bool onOff;
	byte actPos;
	byte cmpPos;
	byte ledNumGrn;
	byte ledNumRed;
};


fbv_pedal fbvPdls[2];




uint8_t ack_received = 0;
uint8_t sense_received = 0;

uint32_t last_ack = 0;

#define CNN_STATE_WAIT_SENSE         0
#define CNN_STATE_WAIT_INITIAL_DATA  1
#define CNN_STATE_REQUEST_PARAMS     2
#define CNN_STATE_RUN                3

#define KEMPER_MODE_TUNER                1
#define KEMPER_MODE_BROWSE               2
#define KEMPER_MODE_PERFORMANCE          3

static int connection_state = CNN_STATE_WAIT_SENSE;

/*
bool mAmp1On = false;
*/
bool mSoloModePostFx = false;
bool previewMode = false;

byte mActPerformance = 0;
byte mNextPerformance = 0;   // for UP/DOWN events
byte mActChannel = 0; // A B C D E=Favorite
int mActPgmNum = 99999;

bool mTunerIsOn = false;

struct sys_ex {				// sysex message container
	char header[5];
	unsigned char fn;
	char id;
	unsigned char data[64];
} sysex_buffer = { { 0x00, 0x20, 0x33, 0x02, 0x7f }, 0, 0, { 0 } };

struct ParamRequest
{
	byte requestType;
	int param;
	bool isExt;
	bool isFx;  // if the parameter represents an FX slot, set the corresponding LED on FBV      
	bool requested;
	bool received;

};

// Paramter Requests to be sent to KPA after receiving Program Change info from the KPA


ParamRequest FX_A_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_A_STATE, false, true };
ParamRequest FX_B_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_B_STATE, false, true };
ParamRequest FX_C_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_C_STATE, false, true };
ParamRequest FX_D_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_D_STATE, false, true };
ParamRequest FX_X_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_X_STATE, false, true };
ParamRequest FX_MOD_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_MOD_STATE, false, true };
ParamRequest FX_DLY_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_DELAY_STATE, false, true };
ParamRequest FX_REV_ON = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_REVERB_STATE, false, true };

ParamRequest FX_A_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_A_TYPE, false, true };
ParamRequest FX_B_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_B_TYPE, false, true };
ParamRequest FX_C_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_C_TYPE, false, true };
ParamRequest FX_D_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_D_TYPE, false, true };
ParamRequest FX_X_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_X_TYPE, false, true };
ParamRequest FX_MOD_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_STOMP_MOD_TYPE, false, true };
ParamRequest FX_DLY_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_DELAY_TYPE, false, true };
ParamRequest FX_REV_TY = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_REVERB_TYPE, false, true };
//ParamRequest GAIN_VAL = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_GAIN_VALUE, false, false };
ParamRequest RIG_COMMENT = { KPA_PARAM_TYPE_STRING, KPA_STRING_ID_RIG_COMMENT, false, false };
ParamRequest LOOPER_STATE = { KPA_PARAM_TYPE_SINGLE, KPA_PARAM_LOOPER_STATE, false, false };

#define PARAM_REQUESTS 18
#define REQ_NUM_LOOPER_STATE 0
ParamRequest mParamRequests[] = {
	LOOPER_STATE,    // must stay in  position 0
	RIG_COMMENT,
	FX_A_TY,
	FX_B_TY,
	FX_C_TY,
	FX_D_TY,
	FX_X_TY,
	FX_MOD_TY,
	FX_DLY_TY,
	FX_REV_TY,
	FX_A_ON,
	FX_B_ON,
	FX_C_ON,
	FX_D_ON,
	FX_X_ON,
	FX_MOD_ON,
	FX_DLY_ON,
	FX_REV_ON,
//	GAIN_VAL,
};



struct FxSlot{
	byte fbv;             // corresponding Switch on FBV
	int paramType;
	int paramState;
	byte contCtl;         // Midi CC Number to send
	bool isEnabled;       // Slot is not empty
	bool isInitialOn;     // On at program change
	bool isOn;            // actual status 
	bool isWah;           // Type for Pedal assignment  
	bool isPitch;         // Type for Pedal assignment
	bool received;

};

#define FX_SLOTS 8
FxSlot mFxSlots[FX_SLOTS];


void fInitFbvPdlValues(){
	fbvPdls[0].ledNumGrn = LINE6FBV_PDL1_GRN;
	fbvPdls[0].ledNumRed = LINE6FBV_PDL1_RED;
	fbvPdls[0].actPos = 0;
	fbvPdls[0].cmpPos = 0;
	fbvPdls[0].onOff = 0;
	fbvPdls[0].ctlNumOff = KPA_CC_WAH;
	fbvPdls[0].ctlNumOn = KPA_CC_GAIN;
	fbvPdls[0].ctlNum = KPA_CC_WAH;


	fbvPdls[1].ledNumGrn = LINE6FBV_PDL2_GRN;
	fbvPdls[1].ledNumRed = LINE6FBV_PDL2_RED;
	fbvPdls[1].actPos = 127;
	fbvPdls[1].cmpPos = 0;
	fbvPdls[1].onOff = 0;
	fbvPdls[1].ctlNumOff = KPA_CC_VOL;
	fbvPdls[1].ctlNumOn = KPA_CC_MORPH;
	fbvPdls[1].ctlNum = KPA_CC_VOL;

	fSetFbvPdlLeds(0);
	fSetFbvPdlLeds(1);

}

void fSetFbvPdlLeds(byte _pdlNum){

	/*
*Pedal LEDs show Controller Type
* | | Green | Red |
*++++++++++++++++++++++++ +
*| none | off | off |
*| Wah | off | flash |
*| Gain | off | on |
*| Vol | on | off |
*| Pitch | flash | off |
*| Morph | flash | flash |
*/

	switch (fbvPdls[_pdlNum].ctlNum){
	case KPA_CC_VOL:
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumGrn, true);
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumRed, false);
		break;
	case KPA_CC_WAH:
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumGrn, false);
		mFbv.setLedFlash(fbvPdls[_pdlNum].ledNumRed, 2000,1000);
		break;
	case KPA_CC_GAIN:
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumGrn, false);
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumRed, true);
		break;
	case KPA_CC_PITCH:
		mFbv.setLedFlash(fbvPdls[_pdlNum].ledNumGrn, 2000, 1000);
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumRed, false);
		break;
	case KPA_CC_MORPH:
		mFbv.setLedFlash(fbvPdls[_pdlNum].ledNumGrn, 2000, 1000);
		mFbv.setLedFlash(fbvPdls[_pdlNum].ledNumRed, 2000, 1000);
		break;
	default:
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumGrn, false);
		mFbv.setLedOnOff(fbvPdls[_pdlNum].ledNumRed, false);

	}

}

void fSwitchFbvPdl(byte _pdlNum){
	fbvPdls[_pdlNum].onOff = (!fbvPdls[_pdlNum].onOff);

	if (fbvPdls[_pdlNum].onOff)
		fbvPdls[_pdlNum].ctlNum = fbvPdls[_pdlNum].ctlNumOn;
	else
		fbvPdls[_pdlNum].ctlNum = fbvPdls[_pdlNum].ctlNumOff;

	fSetFbvPdlLeds(_pdlNum);

}


void fResetFbvPdlValues(uint8_t _pdlNum){
	fbvPdls[0].onOff = 0;
	fbvPdls[0].ctlNumOff = KPA_CC_WAH;
	fbvPdls[0].ctlNumOn = KPA_CC_GAIN;
	fbvPdls[0].ctlNum = fbvPdls[0].ctlNumOff;

	fbvPdls[1].onOff = 0;
	fbvPdls[1].ctlNumOff = KPA_CC_VOL;
	fbvPdls[1].ctlNumOn = KPA_CC_MORPH;
	fbvPdls[1].ctlNum = fbvPdls[1].ctlNumOff;

	

}

void fParseRigComment(){

	byte lCtlNum;
	byte lPdlPos;

	fResetFbvPdlValues(0);
	fResetFbvPdlValues(1);

	lCtlNum = getPdlCtlNum('1', 'A');
	if (lCtlNum){
		fbvPdls[0].ctlNumOn = lCtlNum;
		fbvPdls[0].ctlNum = lCtlNum;
	}

	lCtlNum = getPdlCtlNum('1', 'I');
	if (lCtlNum){
		fbvPdls[0].ctlNumOff = lCtlNum;
	}

	lCtlNum = getPdlCtlNum('2', 'A');
	if (lCtlNum){
		fbvPdls[1].ctlNumOn = lCtlNum;
		fbvPdls[1].ctlNum = lCtlNum;
	}

	lCtlNum = getPdlCtlNum('2', 'I');
	if (lCtlNum){
		fbvPdls[1].ctlNumOff = lCtlNum;
	}


	if (fbvPdls[0].ctlNumOn != KPA_CC_VOL && fbvPdls[1].ctlNumOn != KPA_CC_VOL)
		kpa_sendCtlChange(KPA_CC_VOL, 127); 
	

		lPdlPos = getPdlPos('1');
	if (lPdlPos)
		fbvPdls[0].actPos = lPdlPos;
	
	kpa_sendCtlChange(fbvPdls[0].ctlNum, fbvPdls[0].actPos);

	lPdlPos = getPdlPos('2');
	if (lPdlPos)
		fbvPdls[1].actPos = lPdlPos;

		kpa_sendCtlChange(fbvPdls[1].ctlNum, fbvPdls[1].actPos);
	
	fSetFbvPdlLeds(0);
	fSetFbvPdlLeds(1);


}

char getValueCharForTag(char* _searchTag){

	char *sub;
	int len_searchTag;
	char retval;

	len_searchTag = strlen(_searchTag);
	sub = strstr(mRigComment, _searchTag);

	if (sub)
		retval = sub[len_searchTag];
	else
		retval = 0x00;
	return retval;
}

int getPdlCtlNum(char _pdlNum, char _activeInactive){
	char searchTag[] = "P1A=";

	char pdlChar = 0x00;

	int retval = 0x00;

	searchTag[1] = _pdlNum;
	searchTag[2] = _activeInactive;

	pdlChar = getValueCharForTag(searchTag);

	switch (pdlChar){
	case 'V':
		retval = KPA_CC_VOL;
		break;
	case 'W':
		retval = KPA_CC_WAH;
		break;
	case 'P':
		retval = KPA_CC_PITCH;
		break;
	case 'M':
		retval = KPA_CC_MORPH;
		break;
	case 'G':
		retval = KPA_CC_GAIN;
		break;
	}
	return retval;
}

int getPdlPos(char _pdlNum){
	char searchTag[] = "P1P=";

	char pdlChar;

	int retval;

	searchTag[1] = _pdlNum;

	pdlChar = getValueCharForTag(searchTag);

	switch (pdlChar){
	case 'H':
		retval = 1;
		break;
	case 'T':
		retval = 127;
		break;
	case '1':
		retval = 12;
		break;
	case '2':
		retval = 24;
		break;
	case '3':
		retval = 36;
		break;
	case '4':
		retval = 48;
		break;
	case '5':
		retval = 60;
		break;
	case '6':
		retval = 72;
		break;
	case '7':
		retval = 84;
		break;
	case '8':
		retval = 96;
		break;
	case '9':
		retval = 108;
		break;
	default:
		retval = 0;
	}
	return retval;
}
// set Addresspage, CC number, corresponding FBV Switch for each FX slot
void fInitFxSlots(){
	mFxSlots[FX_SLOT_POS_A].fbv = LINE6FBV_FXLOOP;
	mFxSlots[FX_SLOT_POS_A].paramType = KPA_PARAM_STOMP_A_TYPE;
	mFxSlots[FX_SLOT_POS_A].paramState = KPA_PARAM_STOMP_A_STATE;
	mFxSlots[FX_SLOT_POS_A].contCtl = KPA_CC_FX_A;   // 17

	mFxSlots[FX_SLOT_POS_B].fbv = LINE6FBV_STOMP1;
	mFxSlots[FX_SLOT_POS_B].paramType = KPA_PARAM_STOMP_B_TYPE;
	mFxSlots[FX_SLOT_POS_B].paramState = KPA_PARAM_STOMP_B_STATE;
	mFxSlots[FX_SLOT_POS_B].contCtl = KPA_CC_FX_B;  // 18

	mFxSlots[FX_SLOT_POS_C].fbv = LINE6FBV_STOMP2;
	mFxSlots[FX_SLOT_POS_C].paramType = KPA_PARAM_STOMP_C_TYPE;
	mFxSlots[FX_SLOT_POS_C].paramState = KPA_PARAM_STOMP_C_STATE;
	mFxSlots[FX_SLOT_POS_C].contCtl = KPA_CC_FX_C;  // 19

	mFxSlots[FX_SLOT_POS_D].fbv = LINE6FBV_STOMP3;
	mFxSlots[FX_SLOT_POS_D].paramType = KPA_PARAM_STOMP_D_TYPE;
	mFxSlots[FX_SLOT_POS_D].paramState = KPA_PARAM_STOMP_D_STATE;
	mFxSlots[FX_SLOT_POS_D].contCtl = KPA_CC_FX_D;  // 20

	mFxSlots[FX_SLOT_POS_X].fbv = LINE6FBV_PITCH;
	mFxSlots[FX_SLOT_POS_X].paramType = KPA_PARAM_STOMP_X_TYPE;
	mFxSlots[FX_SLOT_POS_X].paramState = KPA_PARAM_STOMP_X_STATE;
	mFxSlots[FX_SLOT_POS_X].contCtl = KPA_CC_FX_X;  // 22

	mFxSlots[FX_SLOT_POS_MOD].fbv = LINE6FBV_MOD;
	mFxSlots[FX_SLOT_POS_MOD].paramType = KPA_PARAM_STOMP_MOD_TYPE;
	mFxSlots[FX_SLOT_POS_MOD].paramState = KPA_PARAM_STOMP_MOD_STATE;
	mFxSlots[FX_SLOT_POS_MOD].contCtl = KPA_CC_FX_MOD;  // 24

	mFxSlots[FX_SLOT_POS_DLY].fbv = LINE6FBV_DELAY;
	mFxSlots[FX_SLOT_POS_DLY].paramType = KPA_PARAM_DELAY_TYPE;
	mFxSlots[FX_SLOT_POS_DLY].paramState = KPA_PARAM_DELAY_STATE;
	mFxSlots[FX_SLOT_POS_DLY].contCtl = KPA_CC_FX_DLY;  // 27 keep tail (26 cut tail)

	mFxSlots[FX_SLOT_POS_REV].fbv = LINE6FBV_REVERB;
	mFxSlots[FX_SLOT_POS_REV].paramType = KPA_PARAM_REVERB_TYPE;
	mFxSlots[FX_SLOT_POS_REV].paramState = KPA_PARAM_REVERB_STATE;
	mFxSlots[FX_SLOT_POS_REV].contCtl = KPA_CC_FX_REV;  // 29 keep tail (28 cut tail)

	fResetFxSlotValues();

}

// reset all FX Slot Parameters before requesting data from the KPA
void fResetFxSlotValues(){
	for (int i = 0; i < FX_SLOTS; i++){
		mFxSlots[i].isEnabled = false;
		mFxSlots[i].isInitialOn = false;
		mFxSlots[i].isOn = false;
		mFxSlots[i].isWah = false;
		mFxSlots[i].isPitch = false;
		mFxSlots[i].received = false;

	}

	mFxSlots[FX_SLOT_POS_DLY].isEnabled = true;
	mFxSlots[FX_SLOT_POS_REV].isEnabled = true;

}


void fswitchSoloModePostFx(){

	mSoloModePostFx = !mSoloModePostFx;
	mFbv.setLedOnOff(LINE6FBV_AMP2, mSoloModePostFx);

	if (mSoloModePostFx){
		for (size_t i = 4; i < 8; i++)
		{
			if (mFxSlots[i].isEnabled){
				if (!mFxSlots[i].isOn)
				{
					fSwitchFxSlot(i);
				}
			}
		}
	}
	else
	{
		for (size_t i = 4; i < 8; i++)
		{
			if (mFxSlots[i].isOn){
				if (!mFxSlots[i].isInitialOn){
					fSwitchFxSlot(i);
				}
			}
		}
	}
}




// respond to pressed keys on the FBV
void onFbvKeyPressed(byte inKey) {

	// bool switchBank = 0;
	bool switchChannel = 0;
	byte key;

	key = inKey;
	//Serial.println(key, HEX);

	switch (key){
	case LINE6FBV_UP:
		if (kpa_state.mode == KPA_MODE_BROWSE){
			kpa_sendCtlChange(48, 0); // Rig right
			mPauseRequestsUntil = millis() + PAUSE_REQUESTS;
			mSendInitialRequests = true;
			fResetFxSlotValues();
			fResetParamRequests();
		}
		else{
			// switchBank = true;
			previewMode = true;
			kpa_sendCtlChange(48, 1);
			mFbv.setLedFlash(LINE6FBV_UP, FLASH_TIME);
			mFbv.setDisplayFlash((FLASH_TIME / 2), (FLASH_TIME / 4));
			mFbv.setLedOnOff(LINE6FBV_DOWN, false);

		}
		break;
	case LINE6FBV_DOWN:
		if (kpa_state.mode == KPA_MODE_BROWSE){
			kpa_sendCtlChange(49, 0); // Rig left
			mPauseRequestsUntil = millis() + PAUSE_REQUESTS;
			mSendInitialRequests = true;
			fResetFxSlotValues();
			fResetParamRequests();
		}
		else{
			previewMode = true;
			kpa_sendCtlChange(49, 1);
			mFbv.setLedFlash(LINE6FBV_DOWN, FLASH_TIME);
			mFbv.setDisplayFlash((FLASH_TIME / 2), (FLASH_TIME / 4));
			mFbv.setLedOnOff(LINE6FBV_UP, false);
			// switchBank = true;
		}
		break;
	case LINE6FBV_CHANNELA:
	case LINE6FBV_CHANNELB:
	case LINE6FBV_CHANNELC:
	case LINE6FBV_CHANNELD:
	case LINE6FBV_FAVORITE:
		if (kpa_state.mode != KPA_MODE_BROWSE){
			switchChannel = true;
		}
		break;
	case LINE6FBV_TAP:
		kpa_sendCtlChange(KPA_CC_TAP, true);
		break;
	case LINE6FBV_FXLOOP:
		fSwitchFxSlot(FX_SLOT_POS_A);
		break;
	case LINE6FBV_STOMP1:
		fSwitchFxSlot(FX_SLOT_POS_B);
		break;
	case LINE6FBV_STOMP2:
		fSwitchFxSlot(FX_SLOT_POS_C);
		break;
	case LINE6FBV_STOMP3:
		fSwitchFxSlot(FX_SLOT_POS_D);
		break;
	case LINE6FBV_PDL1_SW:
		fSwitchFbvPdl(0);
		break;
	case LINE6FBV_PDL2_SW:
		fSwitchFbvPdl(1);
		break;
	}

	if (!mLooperMode){
		switch (key){
		case LINE6FBV_MOD:
			fSwitchFxSlot(FX_SLOT_POS_MOD);
			break;
		case LINE6FBV_DELAY:
			fSwitchFxSlot(FX_SLOT_POS_DLY);
			break;
		case LINE6FBV_REVERB:
			fSwitchFxSlot(FX_SLOT_POS_REV);
			break;
		case LINE6FBV_PITCH:
			fSwitchFxSlot(FX_SLOT_POS_X);
			break;
		case LINE6FBV_AMP2:
			fswitchSoloModePostFx();
			break;
		}
	}
	else // looperMode
	{
		switch (key)
		{
		case LINE6FBV_AMP1: kpa_sendLooperCmd(KPA_LOOPER_CC_START, true); break;
		case LINE6FBV_AMP2: kpa_sendLooperCmd(KPA_LOOPER_CC_STOP, true); break;
		case LINE6FBV_REVERB: kpa_sendLooperCmd(KPA_LOOPER_CC_TRIGGER, true);  fRequestLooperState(); break;
		case LINE6FBV_PITCH: kpa_sendLooperCmd(KPA_LOOPER_CC_REVERSE, true); break;
		case LINE6FBV_MOD: kpa_sendLooperCmd(KPA_LOOPER_CC_HALFTIME, true); break;
		case LINE6FBV_DELAY: kpa_sendLooperCmd(KPA_LOOPER_CC_UNDOREDO, true); break;
		}

	}

	//if (switchBank){
	//fSetNextPerformanceValue(key);
	//}
	if (switchChannel){
		fChangeProgram(key);
	}

}

void fResetParamRequests(){
	for (size_t i = 0; i < PARAM_REQUESTS; i++)
	{
		mParamRequests[i].received = false;
		mParamRequests[i].requested = false;
	}

}

void fSwitchFxSlot(byte slotNum){
	if (mFxSlots[slotNum].isEnabled == true){
		mFxSlots[slotNum].isOn = !mFxSlots[slotNum].isOn;
		kpa_sendCtlChange(mFxSlots[slotNum].contCtl, mFxSlots[slotNum].isOn);
		fSetLedForFxSlot(slotNum);
	}
	mFbv.syncLedFlash();
}

void fSetLedForFxSlot(byte slotNum){

	if (mFxSlots[slotNum].isEnabled){
		if (mFxSlots[slotNum].isOn){
			mFbv.setLedOnOff(mFxSlots[slotNum].fbv, true);
		}
		else{
			mFbv.setLedFlash(mFxSlots[slotNum].fbv, FLASH_TIME);
		}
	}
	else{
		mFbv.setLedOnOff(mFxSlots[slotNum].fbv, false);
	}
}


void fChangeProgram(byte inKey){
	byte pgmNum;
	uint8_t slotNum = 0;

	pgmNum = mNextPerformance * 5;

	switch (inKey){
	case  LINE6FBV_CHANNELA:
		break; // nothhing to add
	case  LINE6FBV_CHANNELB:
		slotNum = 1;
		break;
	case  LINE6FBV_CHANNELC:
		slotNum = 2;
		break;
	case  LINE6FBV_CHANNELD:
		slotNum = 3;
		break;
	case  LINE6FBV_FAVORITE:
		slotNum = 4;
		break;
	}

	pgmNum += slotNum;
	//if (mActPerformance != mNextPerformance){
	fInitPerformanceSlotNames();
	//}

	//if (mActPgmNum != pgmNum){

	mActPgmNum = pgmNum;    // remember PgmNum
	mActPerformance = mNextPerformance;
	mActChannel = mActPgmNum % 5;
	previewMode = false;
	//kpa_sendPgmChange(pgmNum);  // Pgm Change
	mMidiKpa.sendControlChange(50 + slotNum, 1, KPA_MIDI_CHANNEL);
	mFbv.setDisplayFlash(0, 1);
	fUpdateDisplayInfo();      // update display
	//}
}

void fInitPerformanceSlotNames(){
	for (size_t i = 0; i < 5; i++){
		for (size_t j = 0; j < 16; j++){
			mPerformanceSlotNames[i][j] = 0x00;
		}
	}
}

/*
void fSetNextPerformanceValue(byte inKey){

bool change = false;

switch (inKey){
case LINE6FBV_UP:
if (mNextPerformance < 124){
mNextPerformance++;
mFbv.setLedFlash(LINE6FBV_UP, FLASH_TIME_FAST);
mFbv.setLedOnOff(LINE6FBV_DOWN, false);
change = true;
}
break;
case LINE6FBV_DOWN:
if (mNextPerformance > 0){
mNextPerformance--;
mFbv.setLedFlash(LINE6FBV_DOWN, FLASH_TIME_FAST);
mFbv.setLedOnOff(LINE6FBV_UP, false);
change = true;
}
break;
}

if (change){
previewMode = true;
Serial.print("APP mNextPerformance ");
Serial.println(mNextPerformance);

// Display new bank in the title field
mFbv.setDisplayNumber(mNextPerformance + 1);
mMidiKpa.sendControlChange(47, mNextPerformance, KPA_MIDI_CHANNEL); // Preview Mode aufrufen


if (mNextPerformance != mActPerformance){
mFbv.setDisplayFlash(FLASH_TIME, (FLASH_TIME / 4));
}
else{
mFbv.setLedOnOff(LINE6FBV_DISPLAY, true);
}
}
}
*/
void onFbvKeyReleased(byte inKey, byte inKeyHeld) {

	switch (inKey){

	case LINE6FBV_UP:
		if (kpa_state.mode == KPA_MODE_PERFORM){
			kpa_sendCtlChange(48, 0);
			//mFbv.setLedOnOff(LINE6FBV_UP, false);
		}
		break;
	case LINE6FBV_DOWN:
		if (kpa_state.mode == KPA_MODE_PERFORM){
			kpa_sendCtlChange(49, 0);
			//setLedOnOff(LINE6FBV_DOWN, false);
		}
		break;

	case LINE6FBV_TAP:
		kpa_sendCtlChange(KPA_CC_TAP, false);
		//a_sendParamRequest(KPA_PARAM_TYPE_SINGLE, KPA_ADDR_RIG, KPA_PARAM_NUM_RIG_TEMPO);
		break;
	}
	if (mLooperMode){
		switch (inKey)
		{
		case LINE6FBV_AMP1:   kpa_sendLooperCmd(KPA_LOOPER_CC_START, false); fRequestLooperState(); break;
		case LINE6FBV_AMP2:   kpa_sendLooperCmd(KPA_LOOPER_CC_STOP, false); fRequestLooperState(); break;
		case LINE6FBV_REVERB: kpa_sendLooperCmd(KPA_LOOPER_CC_TRIGGER, false);  fRequestLooperState(); break;
		case LINE6FBV_PITCH:  kpa_sendLooperCmd(KPA_LOOPER_CC_REVERSE, false);  fRequestLooperState(); break;
		case LINE6FBV_MOD:    kpa_sendLooperCmd(KPA_LOOPER_CC_HALFTIME, false);  fRequestLooperState(); break;
		case LINE6FBV_DELAY:  kpa_sendLooperCmd(KPA_LOOPER_CC_UNDOREDO, false);  fRequestLooperState(); break;
		}
	}
}

void fRequestLooperState(){

	Serial.println("fRequestLooperState");
	mParamRequests[REQ_NUM_LOOPER_STATE].received = false;
	mParamRequests[REQ_NUM_LOOPER_STATE].requested = false;
	mPauseRequestsUntil = millis() + PAUSE_REQUESTS_LONG;
	mSendInitialRequests = true;
	mSendNextRequest = true;
}

void onFbvKeyHeld(byte inKey) {

	switch (inKey){
	case LINE6FBV_CHANNELA:
		if (mActChannel == 0)
			mLooperMode = !mLooperMode;
		break;
	case LINE6FBV_CHANNELB:
		if (mActChannel == 1)
			mLooperMode = !mLooperMode;
		break;
	case LINE6FBV_CHANNELC:
		if (mActChannel == 2)
			mLooperMode = !mLooperMode;
		break;
	case LINE6FBV_CHANNELD:
		if (mActChannel == 3)
			mLooperMode = !mLooperMode;
		break;
	case LINE6FBV_FAVORITE:
		if (mActChannel == 4)
			mLooperMode = !mLooperMode;
		break;

	}

	if (mLooperMode)
		mFbv.setDisplayDigit(3, 'L');
	else
		mFbv.setDisplayDigit(3, ' ');
}




// handle MIDI Control Change sent by the FBV
void onFbvCtlChange(byte inCtrl, byte inValue) {

	/*
	Serial.print("FBV CtlChanged ");
	Serial.println(inValue, HEX);
	*/

	if (inCtrl == LINE6FBV_CC_PDL1){
		fbvPdls[0].actPos = inValue;
		if (fbvPdls[0].ctlNum)
		kpa_sendCtlChange(fbvPdls[0].ctlNum, fbvPdls[0].actPos);
	}
	else{
		fbvPdls[1].actPos = inValue;
		if (fbvPdls[1].ctlNum)
			kpa_sendCtlChange(fbvPdls[1].ctlNum, fbvPdls[1].actPos);
	}
}

// in Heartbeat of FBV send Heartbeat to KPA
void onFbvHeartbeat() {

	Serial.println("FBV: Heartbeat");
	kpa_sendBeacon(); // tell KPA that midi controller is connected. needed for bidirectional communication
}


void fUpdateDisplayInfo(){

	/*
	Serial.print("fUpdateDisplayInfo = ");
	Serial.println(mTunerIsOn, HEX);
	*/
	if (mTunerIsOn){
		fDisplayTuner();
	}
	else{
		fDisplayPgmInfo();
	}

}


void fDisplayTuner(){
	char noteName;
	uint8_t noteNum;
	bool flat;
	char* tuneString;
	int tuneValue;



	if ((!kpa_state.noteNum) && (!kpa_state.octave)){
		//Serial.println("Tuner Note: <keine>");
		noteName = ' ';
		flat = false;
	}
	else{
		switch (kpa_state.noteNum)
		{
		case 0:
			noteName = 'C';
			flat = false;
			break;
		case 1:
			noteName = 'D';
			flat = true;
			break;
		case 2:
			noteName = 'D';
			flat = false;
			break;
		case 3:
			noteName = 'E';
			flat = true;
			break;
		case 4:
			noteName = 'E';
			flat = false;
			break;
		case 5:
			noteName = 'F';
			flat = false;
			break;
		case 6:
			noteName = 'G';
			flat = true;
			break;
		case 7:
			noteName = 'G';
			flat = false;
			break;
		case 8:
			noteName = 'A';
			flat = true;
			break;
		case 9:
			noteName = 'A';
			flat = false;
			break;
		case 10:
			noteName = 'B';
			flat = true;
			break;
		case 11:
			noteName = 'B';
			flat = false;
			break;

		default:
			break;
		}
	}


	tuneValue = kpa_state.tune / 128 - 63;  // tuned =  0

	if (noteName == ' ')          tuneString = "I              I";
	else if (tuneValue > 40)    tuneString = "I        ( ( ( I";
	else if (tuneValue > 25)     tuneString = "I        ( (   I";
	else if (tuneValue > 10)     tuneString = "I        (     I";
	else if (tuneValue > 2)      tuneString = "I      **(     I";
	else if ((tuneValue <= 2)
		&& (tuneValue >= -2))  tuneString = "I      **      I";
	else if (tuneValue >= -10) tuneString = "I     )**      I";
	else if (tuneValue >= -25) tuneString = "I   ) )        I";
	else                        tuneString = "I ) ) )        I";


	mFbv.setDisplayDigit(3, noteName);
	mFbv.setDisplayFlat(flat);
	mFbv.setDisplayTitle(tuneString);

#if DEBUG_fDisplayTuner 
	Serial.print("Tuner Note_: ");
	Serial.print(noteName);
	if (flat)
		Serial.print("b >>");
	else
		Serial.print(" >>");
	Serial.print(tuneValue);
	Serial.println(tuneString);
#endif

}






void fDisplayPgmInfo(){

	//Serial.println("fDisplayPgmInfo");

	byte channels[5] = { 0, 0, 0, 0, 0 };

	mFbv.setDisplayNumber(mActPerformance + 1);

	channels[mActChannel] = 1;

	mFbv.setLedOnOff(LINE6FBV_CHANNELA, channels[0]);
	mFbv.setLedOnOff(LINE6FBV_CHANNELB, channels[1]);
	mFbv.setLedOnOff(LINE6FBV_CHANNELC, channels[2]);
	mFbv.setLedOnOff(LINE6FBV_CHANNELD, channels[3]);
	mFbv.setLedOnOff(LINE6FBV_FAVORITE, channels[4]);

	mFbv.setLedOnOff(LINE6FBV_DISPLAY, true);

	if (kpa_state.mode == KPA_MODE_PERFORM){
		//Serial.println(mPerformanceSlotNames[mActChannel]);
		mFbv.setDisplayTitle(mPerformanceSlotNames[mActChannel]);
	}
	else{
		mFbv.setDisplayTitle(mRigName);
	}

	if (!mLooperMode){
		mFbv.setDisplayDigit(3, ' ');
	}
	mFbv.setDisplayFlat(false);
	/*
	Serial.print("fDisplayPgmInfo ");
	Serial.print(channels[0]);
	Serial.print(channels[1]);
	Serial.print(channels[2]);
	Serial.print(channels[3]);
	Serial.println(channels[4]);
	*/
}


void fSendRequestsToKpa(){

	bool allReceived = true;
	bool resend = false;

	if (!mSendInitialRequests)
		return;

	if (millis() < mPauseRequestsUntil)
		return;

	// check if all requests are already received
	for (size_t i = 0; i < PARAM_REQUESTS; i++)
	{
		if (!mParamRequests[i].received){
			allReceived = false;
			i = PARAM_REQUESTS;
		}
	}

	//Serial.println("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
	// if all requests are answered, initial requests are no longer sent
	if (allReceived){
		mSendInitialRequests = false;
		mFbv.syncLedFlash();
		//mFbv.setDisplayTitle(mPerformanceSlotNames[mActChannel]);
		connection_state = CNN_STATE_RUN;
		return;
	}

	//Serial.println("BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB");

	if (!mSendNextRequest){
		if (millis() - mReqestLastSent > 100){
			mReqestLastSent = millis();
			mSendNextRequest = true;
			resend = true;
		}
		else{
			return;
		}
	}

	//Serial.println("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC");


	for (size_t i = 0; i < PARAM_REQUESTS; i++)
	{
		if (!mParamRequests[i].received){
			if ((!mParamRequests[i].requested) || (resend)){
				kpa_sendParamRequest(mParamRequests[i].requestType, mParamRequests[i].param, mParamRequests[i].isExt);
				mSendNextRequest = false;
				mParamRequests[i].requested = true;
				/*
				Serial.print("fSendRequestsToKpa ");
				Serial.print(mParamRequests[i].requestType, HEX);
				Serial.print(" - ");
				Serial.println(mParamRequests[i].param, HEX);
				*/
				i = PARAM_REQUESTS;
			}

		}
	}
}




void onKpaCtlChange(byte chan, byte inCtlNum, byte inCtlVal){
	
		if (previewMode){
			Serial.print("onKpaCtlChanged ");
			Serial.print(inCtlNum, HEX);
			Serial.print(" - ");
			Serial.print(inCtlVal, HEX);
			Serial.println(" ");
		}
		// only Bank Vhange LSB is relevant 
		switch (inCtlNum)
		{
		case CC_BANK_LSB:
			mMidiBank = inCtlVal;
			break;
		
		case 0x2f:
			//Serial.println("Hallo ");
			mFbv.setDisplayNumber(inCtlVal + 1);
			//fSendPerformaceNameRequest();
			break;
		
		default:
			break;
		}
	}

void onKpaPgmChange(byte chan, byte inMidiPgmNum)
{

	if (previewMode){
		Serial.print("onKpaPgmChanged ");
		Serial.print(inMidiPgmNum, HEX);
		Serial.println(" ");
	}

	mMidiPgm = inMidiPgmNum;

	mActPgmNum = (mMidiBank * 128) + mMidiPgm;

	mActChannel = mActPgmNum % 5;
	mNextPerformance = mActPgmNum / 5;
	mActPerformance = mNextPerformance;
	mSoloModePostFx = false;
	mFbv.setLedOnOff(LINE6FBV_AMP2, false);

	mFbv.setLedOnOff(LINE6FBV_UP, false);
	mFbv.setLedOnOff(LINE6FBV_DOWN, false);

	fDisplayPgmInfo();

	// as the KPA sends some parameter infos after Pgm Change
	// wait a bit before starting requests
	mPauseRequestsUntil = millis() + PAUSE_REQUESTS;
	mSendInitialRequests = true;
	fResetFxSlotValues();
	fResetParamRequests();
	fSendSlotNameRequest();
	
}


void fSendPerformaceNameRequest(){
	byte request[] = { 0xF0, 0x00, 0x20, 0x33, 0x00, 0x00, 0x47, 0x00, 0x0, 0x00, 0x01, 0x00, 0, 0xF7 };

	mMidiKpa.sendSysEx(14, request, true);

}



void fSendSlotNameRequest(){
	byte request[] = { 0xF0, 0x00, 0x20, 0x33, 0x00, 0x00, 0x47, 0x00, 0x0, 0x00, 0x01, 0x00, 0 /*slot nUmber*/, 0xF7 };

	request[12] = mActChannel + 1;

	mMidiKpa.sendSysEx(14, request, true);

}


void handle_slot_name_received(char * data, uint8_t slotNum){
	if (kpa_state.mode == KPA_MODE_PERFORM){
		strcpy(mPerformanceSlotNames[slotNum], data);
		if (mActChannel == slotNum){
			fUpdateDisplayInfo();
		}
	}
}

void kpa_handle_param_string(uint32_t param, char * data, unsigned int len){
	bool handled = false;
	char comment[8];

	switch (param)
	{
	case KPA_STRING_ID_RIG_COMMENT:
		strcpy(mRigComment, data);
		Serial.print("RIG Comment ");
		Serial.println(mRigComment); 
		fParseRigComment();
		handled = true;
		break;
	case KPA_STRING_ID_RIG_NAME:
		strcpy(mRigName, data);
		if (kpa_state.mode == KPA_MODE_BROWSE){
			fUpdateDisplayInfo();
		}
		//fSendRigCommentRequest();
		handled = true;
		break;
	case KPA_STRING_ID_PERF_NAME:
		handled = true;
		break;
	case KPA_STRING_ID_PERF_NAME_PREVIEW:
		if (previewMode){
			mFbv.setDisplayTitle((char*)data);
		}
		handled = true;
		break;
	case KPA_STRING_ID_SLOT1_NAME:
		handle_slot_name_received(data, 0);
		handled = true;
		break;
	case KPA_STRING_ID_SLOT2_NAME:
		handle_slot_name_received(data, 1);
		handled = true;
		break;
	case KPA_STRING_ID_SLOT3_NAME:
		handle_slot_name_received(data, 2);
		handled = true;
		break;
	case KPA_STRING_ID_SLOT4_NAME:
		handle_slot_name_received(data, 3);
		handled = true;
		break;
	case KPA_STRING_ID_SLOT5_NAME:
		handle_slot_name_received(data, 4);
		handled = true;
		break;
	}

#if DEBUG_kpa_handle_param_string
	if (!handled){
		Serial.print("kpa_handle_param_string: ");
		Serial.print(param, HEX);
		Serial.print(" - ");
		Serial.print(len);
		Serial.print(" - ");
		Serial.println(data);
	}
#endif
}

void fSetLooperDigit(uint16_t value){
	if (mLooperMode){
		switch (value)
		{
		case 0:
			mFbv.setDisplayDigit(3, 'L');
			break;
		case 1:
			mFbv.setDisplayDigit(3, 'S');
			break;
		case 2:
			mFbv.setDisplayDigit(3, 'T');
			break;
		case 3:
			mFbv.setDisplayDigit(3, 'O');
			break;
		case 4:
			mFbv.setDisplayDigit(3, 'P');
			break;
		case 5:
			mFbv.setDisplayDigit(3, 'R');
			break;
		}

	}

}


void kpa_handle_param_single(uint16_t param, uint16_t value){

	bool handled = false;



	switch (param){
	case KPA_PARAM_CURR_TUNING:
		kpa_state.tune = value;
		handled = true;
		break;
	case KPA_PARAM_CURR_NOTE:
		kpa_state.noteNum = value % 12;
		kpa_state.octave = value / 12;
		handled = true;
		fUpdateDisplayInfo();
		break;
	case KPA_PARAM_TAP_EVENT:
		mFbv.setLedOnOff(LINE6FBV_TAP, value);
		handled = true;
		break;
		case KPA_PARAM_TUNER_STATE:
		mTunerIsOn = (value == 1);
		fUpdateDisplayInfo();
		handled = true;
		break;
	case KPA_PARAM_MODE:
		Serial.print("KPA_PARAM_MODE: ");
		Serial.println(value);
		kpa_state.mode = value;
		fUpdateDisplayInfo();
		handled = true;
		break;
	case KPA_PARAM_LOOPER_STATE:
		//Serial.print("Looooooooooooooooper------>"); Serial.println(value, HEX);
		fSetLooperDigit(value);
		LOOPER_STATE.received = true;
		handled = true;
		break;
	default:
		for (size_t i = 0; i < FX_SLOTS; i++)
		{
			if ((mFxSlots[i].paramType == param) || (mFxSlots[i].paramState == param)){
				for (size_t j = 0; j < PARAM_REQUESTS; j++){
					if (mParamRequests[j].param == param){
						mParamRequests[j].received = true;
						j = PARAM_REQUESTS;
					}
				}

				if (mFxSlots[i].paramType == param){
					mFxSlots[i].isEnabled = (value);
				}
				else{
					mFxSlots[i].isOn = (value);
					if (!mFxSlots[i].received){
						mFxSlots[i].isInitialOn = mFxSlots[i].isOn;
						mFxSlots[i].received = true;
					}
					fSetLedForFxSlot(i);
					if (!mFxSlots[i].isOn){
						mFbv.syncLedFlash();
					}
				}
				handled = true;
				i = FX_SLOTS;
			}
		}
	}

	if (handled){
		mSendNextRequest = true;
	}

#if DEBUG_kpa_handle_param_single
	//if (!handled){
	if (param != KPA_PARAM_TAP_EVENT){
		Serial.print("kpa_handle_param_single: ");
		Serial.print(param, HEX);
		Serial.print(" - ");
		Serial.println(value, HEX);
	}
#endif
}



void onKpaSysEx(byte* data, unsigned len)
{

	static uint8_t last_ack_value;


	bool handled = false;
	static struct sys_ex * s = (struct sys_ex *) (data + 1);
	static int param;
	static int value;
	static int stringSize;


	switch (s->fn) {
	case KPA_SYSEX_FN_RETURN_PARAM:
		param = (s->data[0] << 7) | s->data[1];
		value = (s->data[2] << 7) | s->data[3];
		kpa_handle_param_single(param, value);
		handled = true;
		//case KPA_SYSEX_FN_RETURN_M_PARAM:
		//	break;
	case KPA_SYSEX_FN_RETURN_STRING:
		param = (s->data[0] << 7 | s->data[1]);
		stringSize = strlen((char*)&s->data[2]) + 1; // incl 0x00
		kpa_handle_param_string(param, (char*)&s->data[2], stringSize);
		handled = true;
		break;
		//case: KPA_SYSEX_FN_RETURN_EXT_PARAM
	case KPA_SYSEX_FN_RETURN_EXT_STRING:
		param = (s->data[0] << 28) | (s->data[1] << 21) | (s->data[2] << 14) | s->data[3] << 7 | s->data[4];
		stringSize = strlen((char*)&s->data[5]) + 1; // incl 0x00
		kpa_handle_param_string(param, (char*)&s->data[5], stringSize);
		handled = true;
		break;
	case KPA_SYSEX_FN_ACK:
		if (s->data[0] == 0x7F) {
			if (ack_received && (last_ack_value + 1 != s->data[1])) {
				//                    last_ack = 0;
				//                    ack_received = 0;
			}
			else {
				ack_received = 1;
			}
			last_ack = millis();
			last_ack_value = s->data[1];
		}
		break;


	}

	/*
	if (previewMode){
		Serial.print(s->fn, HEX);
		for (size_t i = 0; i < len; i++){
			Serial.print(s->data[i], HEX);
			Serial.print("-");
		}
	}

	Serial.println(" ");
	*/
#if	DEBUG_onKpaSysEx
	Serial.print("onKpaSysEx ");

	if (!handled){
		Serial.print(s->fn, HEX);
		for (size_t i = 0; i < inSysExSize; i++){
			Serial.print(s->data[i], HEX);
			Serial.print("-");
		}

		Serial.println(" ");
	}

#endif

}

void kpa_sendPgmChange(unsigned inPgmNum){

	byte midiBankNum = inPgmNum / 127;
	byte midiPgmNum = inPgmNum % 127;

	kpa_sendCtlChange(0x00, 0x00);
	kpa_sendCtlChange(0x20, midiBankNum);
	mMidiKpa.sendProgramChange(midiPgmNum, KPA_MIDI_CHANNEL);

}
void kpa_sendCtlChange(byte inCtlNum, byte inCtlVal){
	mMidiKpa.sendControlChange(inCtlNum, inCtlVal, KPA_MIDI_CHANNEL);

}

void kpa_sendOwner(void){

	sysex_buffer.fn = 0x03;
	sysex_buffer.data[0] = 0x7F;
	sysex_buffer.data[1] = 0x7F;
	strcpy((char *)sysex_buffer.data + 2, "WRBI@ORBI_04_04");
	mMidiKpa.sendSysEx(KPA_SYSEX_HEADER_SIZE + 2 + 15, (const byte *)&sysex_buffer, 0);

}
void kpa_sendParamRequest(byte type, int param, bool ext){

	int sz = 2;

	if (!ext) {
		sysex_buffer.fn = type;
		sysex_buffer.data[0] = (param >> 7) & 0xFF;
		sysex_buffer.data[1] = param & 0x7F;
	}
	else {
		sysex_buffer.fn = type;
		sysex_buffer.data[0] = (param >> 28) & 0x7F;
		sysex_buffer.data[1] = (param >> 21) & 0x7F;
		sysex_buffer.data[2] = (param >> 14) & 0x7F;
		sysex_buffer.data[3] = (param >> 07) & 0x7F;
		sysex_buffer.data[4] = (param)& 0x7F;
		sz = 5;
	}

	mMidiKpa.sendSysEx(KPA_SYSEX_HEADER_SIZE + sz, (const byte *)&sysex_buffer, 0);
}

void kpa_setLooperPrePost(uint8_t loc) {
	sysex_buffer.fn = 0x01;
	sysex_buffer.data[0] = 0x7F;
	sysex_buffer.data[1] = 0x35;
	sysex_buffer.data[2] = 0x0;
	sysex_buffer.data[3] = loc;	// 0x0=pre, 0x1=post
	mMidiKpa.sendSysEx(KPA_SYSEX_HEADER_SIZE + 4, (const byte *)&sysex_buffer, 0);
}

void kpa_sendBeacon(void){

	//byte cnnStr[] = { 0xF0, 0x00, 0x20, 0x33, 0x02, 0x7F, 0x7E, 0x00, 0x40, 0x01, 0x36, 0x04, 0xF7 };
	byte cnnStr[] = { 0xF0, 0x00, 0x20, 0x33, 0x02, 0x7F, 0x7E, 0x00, 0x40, 0x03, 0x36, 0x04, 0xF7 };

	mMidiKpa.sendSysEx(13, cnnStr, 1);



	/*
	sysex_buffer.fn = 0x7E;
	sysex_buffer.data[0] = 0x40;
	sysex_buffer.data[1] = 2;
	sysex_buffer.data[2] = 0x6E | (ack_received ? 0 : 1);
	sysex_buffer.data[3] = 5;
	mMidiKpa.sendSysEx(KPA_SYSEX_HEADER_SIZE + 4, (const byte *)&sysex_buffer, 0);
	*/
	/*
	sysex_buffer.fn = 0x7e;
	sysex_buffer.data[0] = 0x40;
	sysex_buffer.data[1] = 3;	// Set 3
	sysex_buffer.data[2] = 0x2e | (ack_received ? 0 : 1); // flags
	sysex_buffer.data[3] = 5;	// 5 * 2 sec time lease
	mMidiKpa.sendSysEx(KPA_SYSEX_HEADER_SIZE + 4, (const byte *)&sysex_buffer, 0);
	*/
}


void kpa_reset_sysex(){
	sysex_buffer = { { 0x00, 0x20, 0x33, 0x02, 0x7F }, 0, 0, { 0 }, };
}

void kpa_sendLooperCmd(byte inCmd, byte inKeyPress){
	kpa_sendCtlChange(0x63, 0x7D);
	kpa_sendCtlChange(0x62, inCmd);
	kpa_sendCtlChange(0x06, 0x00);
	kpa_sendCtlChange(0x26, inKeyPress);
}

void fSetTapLed(){

	//Serial.print("fSetTapLed "); Serial.println(mBPM);
	int ms;

	if (mBPM == 0){
		mFbv.setLedOnOff(LINE6FBV_TAP, false);
	}
	else{
		ms = 60000 / mBPM;
		mFbv.setLedFlash(LINE6FBV_TAP, ms);
	}


}

void onKpaSense(void){
	sense_received = true;
}

void kpa_process(){
	static long last_sent = 0;
	static char current_slot = 0;

	switch (connection_state) {
	case CNN_STATE_WAIT_SENSE:
		if (sense_received) {
			connection_state = CNN_STATE_WAIT_INITIAL_DATA;
		}
	case CNN_STATE_WAIT_INITIAL_DATA:
		if (millis() - last_sent > 2000) {
			kpa_sendOwner();
			kpa_sendBeacon();
			last_sent = millis();
		}

		if (ack_received) {
			current_slot = 0;
			//connection_state = KPA_STATE_REQUEST_PARAMS;
			connection_state = CNN_STATE_RUN;
		}
		break;
		/*
		case KPA_STATE_REQUEST_PARAMS:
		if (current_slot < 5) {
		if (millis() - last_sent > 200) {
		last_sent = millis();
		request_param(0x4000 + current_slot++);
		}
		}
		else {
		current_slot = 0;
		connection_state = KPA_STATE_RUN;
		}
		break;
		*/
	case CNN_STATE_RUN:
		if (millis() - last_sent > 5000) {
			kpa_sendBeacon();
			last_sent = millis();
		}
		if (millis() - last_ack > 5000) {
			connection_state = CNN_STATE_WAIT_INITIAL_DATA;
			ack_received = 0;
			sense_received = 0;
		}
		break;
	}
}

void setup() {
	// Status LED
	pinMode(13, OUTPUT);
	digitalWrite(13, HIGH);

	Serial.begin(115200);
	Serial.println("los gehts...");

	mFbv.begin(&SERIAL_FBV); // open port 

	// set callback functions for the FBV
	mFbv.setHandleKeyPressed(&onFbvKeyPressed);
	mFbv.setHandleKeyReleased(&onFbvKeyReleased);
	mFbv.setHandleKeyHeld(&onFbvKeyHeld);
	mFbv.setHandleHeartbeat(&onFbvHeartbeat);
	mFbv.setHandleCtrlChanged(&onFbvCtlChange);

	mFbv.setHoldTime(LINE6FBV_CHANNELA, FBV_HOLD_TIME);
	mFbv.setHoldTime(LINE6FBV_CHANNELB, FBV_HOLD_TIME);
	mFbv.setHoldTime(LINE6FBV_CHANNELC, FBV_HOLD_TIME);
	mFbv.setHoldTime(LINE6FBV_CHANNELD, FBV_HOLD_TIME);
	mFbv.setHoldTime(LINE6FBV_FAVORITE, FBV_HOLD_TIME);

	mFbv.setLedOnOff(LINE6FBV_DISPLAY, 1);
	mFbv.setDisplayTitle("WRBI(AT)ORBI");
	mFbv.updateUI();

	connection_state = CNN_STATE_WAIT_SENSE;
	mMidiKpa.begin();
	mMidiKpa.setInputChannel(KPA_MIDI_CHANNEL);
	mMidiKpa.turnThruOff();

	mMidiKpa.setHandleActiveSensing(onKpaSense);
	mMidiKpa.setHandleControlChange(onKpaCtlChange);
	mMidiKpa.setHandleProgramChange(onKpaPgmChange);
	mMidiKpa.setHandleSystemExclusive(onKpaSysEx);

	fInitFxSlots();
	fInitFbvPdlValues();
	Serial.println("fertsch");

}



void loop() {

	mFbv.read();  // Receive Commands from FBV

	mMidiKpa.read();  // Receive Information from KPA

	kpa_process();
	fSendRequestsToKpa();

	mFbv.updateUI();  // update Display and LEDs on the FBV to the values set in the setDisplay..... routines
}
