/*
 *  AppleTAS3004Audio.cpp (definition)
 *  Project : AppleLegacyAudio
 *
 *  Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 *  @APPLE_LICENSE_HEADER_START@
 *  
 *  Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 *  
 *  This file contains Original Code and/or Modifications of Original Code
 *  as defined in and that are subject to the Apple Public Source License
 *  Version 2.0 (the 'License'). You may not use this file except in
 *  compliance with the License. Please obtain a copy of the License at
 *  http://www.opensource.apple.com/apsl/ and read it before using this
 *  file.
 *  
 *  The Original Code and all software distributed under the License are
 *  distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 *  EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 *  INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 *  Please see the License for the specific language governing rights and
 *  limitations under the License.
 *  
 *  @APPLE_LICENSE_HEADER_END@
 *
 *  Hardware independent (relatively) code for the Texas Insruments TAS3004 Codec
 *  NEW-WORLD MACHINE ONLY!!!
 */

#include "AppleTAS3004Audio.h"

#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>

#include "AudioHardwareConstants.h"
#include "AppleTASEQPrefs.cpp"
#include "AudioHardwareUtilities.h"

#define super IOService

OSDefineMetaClassAndStructors(AppleTAS3004Audio, AudioHardwareObjectInterface)

const UInt8 		AppleTAS3004Audio::kDEQAddress = 0x6A;
const EQPrefsPtr 	AppleTAS3004Audio::kEQPrefsPtr = &theEQPrefs;

//-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

#pragma mark +UNIX LIKE FUNCTIONS

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ::init()
// call into superclass and initialize.
bool AppleTAS3004Audio::init(OSDictionary *properties)
{
	debugIOLog("+ AppleTAS3004Audio::init\n");
	if (!super::init(properties))
		return false;

	mVolLeft = 0;
	mVolRight = 0;
	mVolMuteActive = false;

	debugIOLog("- AppleTAS3004Audio::init\n");
	return true;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
bool AppleTAS3004Audio::start (IOService * provider) {
	bool					result;

	debug3IOLog ("+ AppleTAS3004Audio[%p]::start(%p)\n", this, provider);

	FailIf (!provider, Exit);
	mAudioDeviceProvider = (AppleOnboardAudio *)provider;
	
	result = super::start (provider);

	result = provider->open (this, kFamilyOption_OpenMultiple);

Exit:
	debug4IOLog ("- AppleTAS3004Audio[%p]::start(%p) returns %s\n", this, provider, result == true ? "true" : "false");
	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void AppleTAS3004Audio::free()
{
	debugIOLog("+ AppleTAS3004Audio::free\n");

	super::free();

	debugIOLog("- AppleTAS3004Audio::free\n");
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Post init init, but pre preDMAEngineInit init - sets platform object, possibly transport object
void AppleTAS3004Audio::initPlugin(PlatformInterface* inPlatformObject) 
{
	mPlatformInterface = inPlatformObject;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
bool AppleTAS3004Audio::willTerminate ( IOService * provider, IOOptionBits options )
{
	bool result = super::willTerminate( provider, options );
	debug4IOLog("AppleTAS3004Audio::willTerminate(%p) returns %d, mAudioDeviceProvider = %p\n", provider, result, mAudioDeviceProvider);

	if (provider == mAudioDeviceProvider) {
		debugIOLog ("closing our provider\n");
		provider->close (this);
	}

	debug4IOLog ("- AppleTAS3004Audio[%p]::willTerminate(%p) returns %s\n", this, provider, result == true ? "true" : "false");
	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
bool AppleTAS3004Audio::requestTerminate ( IOService * provider, IOOptionBits options )
{

	
	bool result = super::requestTerminate( provider, options );
	debug4IOLog("AppleTAS3004Audio::requestTerminate(%p) returns %d, mAudioDeviceProvider = %p\n", provider, result, mAudioDeviceProvider);
	return result;
}

/************************** Hardware Register Manipulation ********************/
// Hardware specific functions : These are all virtual functions and we have to 
// implement these in the driver class

// --------------------------------------------------------------------------
// ::sndHWInitialize
// hardware specific initialization needs to be in here, together with the code
// required to start audio on the device.
//
//	There are three sections of memory mapped I/O that are directly accessed by the AppleLegacyAudio.  These
//	include the GPIOs, I2S DMA Channel Registers and I2S control registers.  They fall within the memory map 
//	as follows:
//	~                              ~
//	|______________________________|
//	|                              |
//	|         I2S Control          |
//	|______________________________|	<-	soundConfigSpace = ioBase + i2s0BaseOffset ...OR... ioBase + i2s1BaseOffset
//	|                              |
//	~                              ~
//	~                              ~
//	|______________________________|
//	|                              |
//	|       I2S DMA Channel        |
//	|______________________________|	<-	i2sDMA = ioBase + i2s0_DMA ...OR... ioBase + i2s1_DMA
//	|                              |
//	~                              ~
//	~                              ~
//	|______________________________|
//	|            FCRs              |
//	|            GPIO              |	<-	gpio = ioBase + gpioOffsetAddress
//	|         ExtIntGPIO           |	<-	fcr = ioBase + fcrOffsetAddress
//	|______________________________|	<-	ioConfigurationBaseAddress
//	|                              |
//	~                              ~
//
//	The I2S DMA Channel is mapped in by the AppleLegacyDBDMAAudioDMAEngine.  Only the I2S control registers are 
//	mapped in by the AudioI2SControl.  The Apple I/O Configuration Space (i.e. FCRs, GPIOs and ExtIntGPIOs)
//	are mapped in by the subclass of AppleLegacyAudio.  The FCRs must also be mapped in by the AudioI2SControl
//	object as the init method must enable the I2S I/O Module for which the AudioI2SControl object is
//	being instantiated for.
//
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// ::initHardware
// Don't do a whole lot in here, but do call the inherited inithardware method.
// in turn this is going to call sndHWInitialize to perform initialization.	 All
// of the code to initialize and start the device needs to be in that routine, this 
// is kept as simple as possible.
bool AppleTAS3004Audio::preDMAEngineInit()
{
	IOReturn				err;
	IORegistryEntry			*sound;
	UInt32					loopCnt;
	UInt8					data[kTAS3004BIQwidth];						// space for biggest register size

	debugIOLog("+ AppleTAS3004Audio::preDMAEngineInit\n");

	debug2IOLog ("ourProvider's name is %s\n", mAudioDeviceProvider->getName ());
	sound = mAudioDeviceProvider->getParentEntry (gIOServicePlane);
	FailIf (!sound, Exit);
	debug2IOLog ("sound's name is %s\n", sound->getName ());

	//	Determine which systems to exclude from the default behavior of releasing the headphone
	//	mute after 200 milliseconds delay [2660341].  Typically this is done for any non-portable
	//	CPU.  Portable CPUs will achieve better battery life by leaving the mute asserted.  Desktop
	//	CPUs have a different amplifier configuration and only want the amplifier quiet during a
	//	detect transition.
	
	drc.compressionRatioNumerator	= kDrcRatioNumerator;
	drc.compressionRatioDenominator	= kDrcRationDenominator;
	drc.threshold					= kDrcThresholdMax;
	drc.maximumVolume				= kDefaultMaximumVolume;
	drc.enable						= false;

	//	Initialize the TAS3004 as follows:
	//		Mode:					normal
	//		SCLK:					64 fs
	//		input serial mode:		i2s
	//		output serial mode:		i2s
	//		serial word length:		16 bits
	//		Dynamic range control:	disabled
	//		Volume (left & right):	muted
	//		Treble / Bass:			unity
	//		Biquad filters:			unity
	//	Initialize the TAS3004 registers the same as the TAS3004 with the following additions:
	//		AnalogPowerDown:		normal
	//		
	data[0] = ( kNormalLoad << kFL ) | ( k64fs << kSC ) | TAS_I2S_MODE | ( TAS_WORD_LENGTH << kW0 );
	CODEC_WriteRegister( kTAS3004MainCtrl1Reg, data, kUPDATE_SHADOW );	//	default to normal load mode, 16 bit I2S

	data[DRC_AboveThreshold]	= kDisableDRC;
	data[DRC_BelowThreshold]	= kDRCBelowThreshold1to1;
	data[DRC_Threshold]			= kDRCUnityThreshold;
	data[DRC_Integration]		= kDRCIntegrationThreshold;
	data[DRC_Attack]			= kDRCAttachThreshold;
	data[DRC_Decay]				= kDRCDecayThreshold;
	CODEC_WriteRegister( kTAS3004DynamicRangeCtrlReg, data, kUPDATE_SHADOW );

	for( loopCnt = 0; loopCnt < kTAS3004VOLwidth; loopCnt++ )				//	init to volume = muted
	    data[loopCnt] = 0;
	CODEC_WriteRegister( kTAS3004VolumeCtrlReg, data, kUPDATE_SHADOW );

	data[0] = 0x72;									//	treble = bass = unity 0.0 dB
	CODEC_WriteRegister( kTAS3004TrebleCtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004BassCtrlReg, data, kUPDATE_SHADOW );

	data[0] = 0x10;								//	output mixer output channel to unity = 0.0 dB
	data[1] = 0x00;
	data[2] = 0x00;
	data[3] = 0x00;								//	output mixer call progress channel to mute = -70.0 dB
	data[4] = 0x00;
	data[5] = 0x00;
	data[6] = 0x00;								//	output mixer analog playthrough channel to mute = -70.0 dB
	data[7] = 0x00;
	data[8] = 0x00;
	CODEC_WriteRegister( kTAS3004MixerLeftGainReg, data, kUPDATE_SHADOW );	//	initialize left channel
	CODEC_WriteRegister( kTAS3004MixerRightGainReg, data, kUPDATE_SHADOW );	//	initialize right channel

	for( loopCnt = 1; loopCnt < kTAS3004BIQwidth; loopCnt++ )				//	all biquads to unity gain all pass mode
		data[loopCnt] = 0x00;
	data[0] = 0x10;

	CODEC_WriteRegister( kTAS3004LeftBiquad0CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004LeftBiquad1CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004LeftBiquad2CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004LeftBiquad3CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004LeftBiquad4CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004LeftBiquad5CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004LeftBiquad6CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004LeftLoudnessBiquadReg, data, kUPDATE_SHADOW );

	CODEC_WriteRegister( kTAS3004RightBiquad0CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightBiquad1CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightBiquad2CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightBiquad3CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightBiquad4CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightBiquad5CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightBiquad6CtrlReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightLoudnessBiquadReg, data, kUPDATE_SHADOW );
	
	data[0] = 0x00;	//	loudness gain to mute
	CODEC_WriteRegister( kTAS3004LeftLoudnessBiquadGainReg, data, kUPDATE_SHADOW );
	CODEC_WriteRegister( kTAS3004RightLoudnessBiquadGainReg, data, kUPDATE_SHADOW );
	
	data[0] = ( kADMNormal << kADM ) | ( kDeEmphasisOFF << kADM ) | ( kPowerDownAnalog << kAPD );
	CODEC_WriteRegister( kTAS3004AnalogControlReg, data, kUPDATE_SHADOW );
	
	data[0] = ( kAllPassFilter << kAP ) | ( kNormalBassTreble << kDL );
	CODEC_WriteRegister( kTAS3004MainCtrl2Reg, data, kUPDATE_SHADOW );


	err = CODEC_Initialize();			//	flush the shadow contents to the HW
	IOSleep (1);
	ToggleAnalogPowerDownWake();

	minVolume = kMinimumVolume;
	maxVolume = kMaximumVolume + drc.maximumVolume;

Exit:

	debugIOLog("- AppleTAS3004Audio::preDMAEngineInit\n");
	return true;
}

// --------------------------------------------------------------------------
void AppleTAS3004Audio::postDMAEngineInit () {

	DEBUG_IOLOG("+ AppleTAS3004Audio::sndHWPostDMAEngineInit\n");
	
	CODEC_LogRegisters();
	
	DEBUG_IOLOG("- AppleTAS3004Audio::sndHWPostDMAEngineInit\n");
	return;
}


// --------------------------------------------------------------------------
IOReturn	AppleTAS3004Audio::SetAnalogPowerDownMode( UInt8 mode )
{
	IOReturn	err;
	UInt8		dataBuffer[kTAS3004ANALOGCTRLREGwidth];
	
	err = kIOReturnSuccess;
	if ( kPowerDownAnalog == mode || kPowerNormalAnalog == mode )
	{
		err = CODEC_ReadRegister( kTAS3004AnalogControlReg, dataBuffer );
		if ( kIOReturnSuccess == err )
		{
			dataBuffer[0] &= ~( kAPD_MASK << kAPD );
			dataBuffer[0] |= ( mode << kAPD );
			err = CODEC_WriteRegister( kTAS3004AnalogControlReg, dataBuffer, kUPDATE_ALL );
		}
	}
	return err;
}

// --------------------------------------------------------------------------
IOReturn	AppleTAS3004Audio::ToggleAnalogPowerDownWake( void )
{
	IOReturn	err;
	
	err = SetAnalogPowerDownMode (kPowerDownAnalog);
	if (kIOReturnSuccess == err) {
		err = SetAnalogPowerDownMode (kPowerNormalAnalog);
	}
	return err;
}

#pragma mark +HARDWARE IO ACTIVATION
/************************** Manipulation of input and outputs ***********************/
/********(These functions are enough to implement the simple UI policy)**************/

// --------------------------------------------------------------------------
UInt32	AppleTAS3004Audio::getActiveOutput(void)
{
	DEBUG_IOLOG("+ AppleTAS3004Audio::sndHWGetActiveOutputExclusive\n");
	DEBUG_IOLOG("- AppleTAS3004Audio::sndHWGetActiveOutputExclusive\n");
	return 0;
}

// --------------------------------------------------------------------------
IOReturn   AppleTAS3004Audio::setActiveOutput(UInt32 outputPort )
{
	DEBUG_IOLOG("+ AppleTAS3004Audio::sndHWSetActiveOutputExclusive\n");

	IOReturn myReturn = kIOReturnSuccess;

	DEBUG_IOLOG("- AppleTAS3004Audio::sndHWSetActiveOutputExclusive\n");
	return(myReturn);
}

// --------------------------------------------------------------------------
UInt32	AppleTAS3004Audio::getActiveInput(void)
{
	DEBUG_IOLOG("+ AppleTAS3004Audio::sndHWGetActiveInputExclusive\n");
	DEBUG_IOLOG("- AppleTAS3004Audio::sndHWGetActiveInputExclusive\n");
	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Set the hardware to select the desired input port after validating
//	that the target input port is available. 
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	The TAS3004 supports mutually exclusive selection of one of four analog
//	input signals.  There is no provision for selection of a 'none' input.
//	Mapping of input selections is as follows:
//		kSndHWInput1:		TAS3004 analog input A, Stereo
//		kSndHWInput2:		TAS3004 analog input B, Stereo
//		kSndHWInput3:		TAS3004 analog input B, Mono sourced from left
//		kSndHWInput4:		TAS3004 analog input B, Mono sourced from right
//	Only a subset of the available inputs are implemented on any given CPU
//	due to the multiple modes that analog input B can operate in.  The
//	'sound-objects' describes the resident hardware implemenation regarding
//	available inputs and the type of input.  It is possible to achieve an
//	equivalent selection of none by collecting the connections to the analog
//	ports.  If the TAS3004 stereo input A is unused then kSndHWInput1 may
//	be aliased as kSndHWInputNone.  If neither of the TAS3004 input B
//	ports are used then kSndHWInput2 may be aliased as kSndHWInputNone.  If
//	one of the TAS3004 input B ports is used as a mono input port but the 
//	other input B mono input port remains unused then the unused mono input
//	port (i.e. kSndHWInput3 for the left channel or kSndHWInput4 for the
//	right channel) may be aliased as kSndHWInputNone.
IOReturn   AppleTAS3004Audio::setActiveInput (UInt32 input)
{
    UInt8		data[kTAS3004MaximumRegisterWidth];
    IOReturn	result = kIOReturnSuccess; 
    
	debug2IOLog("+ AppleTAS3004Audio::setActiveInput (%4s)\n", (char *)&input);

	//	Mask off the current input selection and then OR in the new selections
	CODEC_ReadRegister (kTAS3004AnalogControlReg, data);
	data[0] &= ~((1 << kADM) | (1 << kLRB) | (1 << kINP));
    switch (input) {
      case kIOAudioInputPortSubTypeLine:
			data[0] |= ((kADMNormal << kADM) | (kLeftInputForMonaural << kLRB) | (kAnalogInputA << kINP));
			result = CODEC_WriteRegister (kTAS3004AnalogControlReg, data, kUPDATE_ALL);
			break;
        case kIOAudioInputPortSubTypeInternalMicrophone:
			data[0] |= ((kADMBInputsMonaural << kADM) | (kRightInputForMonaural << kLRB) | (kAnalogInputB << kINP));
			result = CODEC_WriteRegister (kTAS3004AnalogControlReg, data, kUPDATE_ALL);
			break;
        default:			
			result = kIOReturnError;																					
			break;
		//case kSndHWInput2:	data[0] |= ((kADMNormal << kADM) | (kLeftInputForMonaural << kLRB) | (kAnalogInputB << kINP));			break;
        //case kSndHWInput3:	data[0] |= ((kADMBInputsMonaural << kADM) | (kLeftInputForMonaural << kLRB) | (kAnalogInputB << kINP));	break;
    }

    debugIOLog("- AppleTAS3004Audio::setActiveInput\n");
    return(result);
}

#pragma mark +CONTROL FUNCTIONS
// control function

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
bool AppleTAS3004Audio::getMute()
{
	return mVolMuteActive;
}

// --------------------------------------------------------------------------
IOReturn AppleTAS3004Audio::setMute(bool mutestate)
{
	IOReturn						result;

	result = kIOReturnSuccess;

	DEBUG_IOLOG("+ AppleTAS3004Audio::setMute\n");

	if (true == mutestate) {
		if (false == mVolMuteActive) {
			// mute the part
			mVolMuteActive = mutestate ;
			result = SetVolumeCoefficients (0, 0);
		}
	} else {
		// unmute the part
		mVolMuteActive = mutestate ;
		result = SetVolumeCoefficients (volumeTable[(UInt32)mVolLeft], volumeTable[(UInt32)mVolRight]);
	}
	
	CODEC_LogRegisters();

	DEBUG_IOLOG ("- AppleTAS3004Audio::setMute\n");
	return (result);
}

// --------------------------------------------------------------------------
UInt32 AppleTAS3004Audio::getMinimumdBVolume () {
	return volumedBTable[minVolume];
}

// --------------------------------------------------------------------------
UInt32 AppleTAS3004Audio::getMaximumdBVolume () {
	return volumedBTable[maxVolume];
}

// --------------------------------------------------------------------------
UInt32 AppleTAS3004Audio::getMinimumVolume () {
	return minVolume;
}

// --------------------------------------------------------------------------
UInt32 AppleTAS3004Audio::getMaximumVolume () {
	return maxVolume;
}

UInt32 AppleTAS3004Audio::getMaximumdBGain (void) {
	return kTAS3004_Plus_12dB;
}

UInt32 AppleTAS3004Audio::getMinimumdBGain (void) {
	return kTAS3004_Minus_12dB;
}

UInt32 AppleTAS3004Audio::getMaximumGain (void) {
	return kTAS3004_MaxGainSteps;
}

UInt32 AppleTAS3004Audio::getMinimumGain (void) {
	return 0;
}

// --------------------------------------------------------------------------
bool AppleTAS3004Audio::setVolume(UInt32 leftVolume, UInt32 rightVolume)
{
	bool					result;

	result = false;

	DEBUG3_IOLOG("+ AppleTAS3004Audio::sndHWSetSystemVolume (left: %ld, right %ld)\n", leftVolume, rightVolume);
	mVolLeft = leftVolume;
	mVolRight = rightVolume;
	result = SetVolumeCoefficients (volumeTable[(UInt32)mVolLeft], volumeTable[(UInt32)mVolRight]);

	DEBUG_IOLOG("- AppleTAS3004Audio::sndHWSetSystemVolume\n");
	return (result == kIOReturnSuccess);
}

// --------------------------------------------------------------------------
IOReturn AppleTAS3004Audio::setPlayThrough(bool playthroughstate)
{
	UInt8		leftMixerGain[kTAS3004MIXERGAINwidth];
	UInt8		rightMixerGain[kTAS3004MIXERGAINwidth];
	IOReturn	err;
	
	debugIOLog("+ AppleTAS3004Audio::sndHWSetPlayThrough\n");

	err = CODEC_ReadRegister ( kTAS3004MixerLeftGainReg, leftMixerGain );
	FailIf ( kIOReturnSuccess != err, Exit );

	err = CODEC_ReadRegister ( kTAS3004MixerRightGainReg, rightMixerGain );
	FailIf ( kIOReturnSuccess != err, Exit );

	if ( playthroughstate ) {
		leftMixerGain[6] = rightMixerGain[6] = 0x10;
	} else {
		leftMixerGain[6] = rightMixerGain[6] = 0x00;
	}
	err = CODEC_WriteRegister ( kTAS3004MixerLeftGainReg, leftMixerGain, kUPDATE_ALL );
	err = CODEC_WriteRegister ( kTAS3004MixerRightGainReg, rightMixerGain, kUPDATE_ALL );

Exit:
	debugIOLog("- AppleTAS3004Audio::sndHWSetPlayThrough\n");
	return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn	AppleTAS3004Audio::setSampleRate ( UInt32 sampleRate ) {
	IOReturn		result = kIOReturnBadArgument;
	
	FailIf ( !( 44100 == sampleRate || 48000 == sampleRate || 32000 == sampleRate ), Exit );
	CODEC_Initialize();
	result = kIOReturnSuccess;
Exit:
	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn	AppleTAS3004Audio::setSampleDepth ( UInt32 sampleDepth ) {
	UInt8			data;
	IOReturn		result = kIOReturnBadArgument;
	
	debug2IOLog ( "+ AppleTAS3004Audio:: setSampleDepth ( %d )\n", (unsigned int) sampleDepth );
	FailIf ( !( 16 == sampleDepth || 24 == sampleDepth ), Exit );
	
	CODEC_Initialize ();
	
	result = CODEC_ReadRegister ( kTAS3004MainCtrl1Reg, &data );
	FailIf ( kIOReturnSuccess != result, Exit );
	
	data &= ~( kSerialWordLengthMASK << kW0 ); 
	
	if ( 16 == sampleDepth ) {
		data |= ( kSerialWordLength16 << kW0 );
	} else {
		data |= ( kSerialWordLength24 << kW0 );
	} 
	
	result = CODEC_WriteRegister ( kTAS3004MainCtrl1Reg, &data, kFORCE_UPDATE_ALL );
	
Exit:
	debug3IOLog ( "+ AppleTAS3004Audio::setSampleWidth ( %d ) returns %d\n", (unsigned int)sampleDepth, (unsigned int)result );
	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn	AppleTAS3004Audio::breakClockSelect ( UInt32 clockSource ) {
	IOReturn		result;
	
	result = kIOReturnSuccess;
	FailIf ( !( kTRANSPORT_MASTER_CLOCK == clockSource || kTRANSPORT_SLAVE_CLOCK == clockSource ), Exit );
	result = kIOReturnSuccess;
Exit:
	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn	AppleTAS3004Audio::makeClockSelect ( UInt32 clockSource ) {
	IOReturn		result;
	
	result = kIOReturnSuccess;
	FailIf ( !( kTRANSPORT_MASTER_CLOCK == clockSource || kTRANSPORT_SLAVE_CLOCK == clockSource ), Exit );
	result = kIOReturnSuccess;
Exit:
	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Fatal error recovery 
void AppleTAS3004Audio::recoverFromFatalError ( FatalRecoverySelector selector ) {

	debug2IOLog ( "+ AppleTAS3004Audio::recoverFromFatalError ( %d )\n", (unsigned int)selector );

	if (semaphores ) { debugIOLog ( "REDUNDANT RECOVERY FROM FATAL ERROR\n" ); }
	FailIf ( NULL == mPlatformInterface, Exit );
	
	switch ( selector ) {
		case kControlBusFatalErrorRecovery:
			CODEC_Initialize();
			break;
		case kClockSourceInterruptedRecovery:
			CODEC_Initialize();
			break;
		default:
			debugIOLog ( "*** Requested recovery from unknown condition!\n" );
			break;
	}
Exit:
	debug2IOLog ( "- AppleTAS3004Audio::recoverFromFatalError ( %d )\n", (unsigned int)selector );
	return;
}

UInt32 AppleTAS3004Audio::getCurrentSampleFrame (void) {
	return mPlatformInterface->getFrameCount ();
}

void AppleTAS3004Audio::setCurrentSampleFrame (UInt32 value) {
	if ( kIOReturnSuccess != mPlatformInterface->setFrameCount (value) ) {
		debugIOLog ( "*** AppleTAS3004Audio::setCurrentSampleFrame FAILED\n" );
	}
}

#pragma mark +POWER MANAGEMENT

// --------------------------------------------------------------------------
//	Set the audio hardware to sleep mode by placing the TAS3004 into
//	analog power down mode after muting the amplifiers.  The I2S clocks
//	must also be stopped after these two tasks in order to achieve
//	a fully low power state.  Some CPU implemenations implement a
//	Codec RESET by ANDing the mute states of the internal speaker
//	amplifier and the headphone amplifier.  The Codec must be put 
//	into analog power down state prior to asserting the both amplifier
//	mutes.  This is only invoked when the 'has-anded-reset' property
//	exists with a value of 'true'.  For all other cases, the original
//	signal manipulation order exists.
IOReturn AppleTAS3004Audio::performDeviceSleep () {
//	IOService *							keyLargo;

	debugIOLog ("+ AppleTAS3004Audio::performDeviceSleep\n");

//	keyLargo = NULL;

	//	Mute all of the amplifiers
        
    SetAnalogPowerDownMode (kPowerDownAnalog);
 
	mPlatformInterface->setHeadphoneMuteState ( kGPIO_Muted );
	mPlatformInterface->setSpeakerMuteState ( kGPIO_Muted );
	mPlatformInterface->setLineOutMuteState ( kGPIO_Muted );
    
	IOSleep (kAmpRecoveryMuteDuration);

	debugIOLog ("- AppleTAS3004Audio::performDeviceSleep\n");
	return kIOReturnSuccess;
}
	
// --------------------------------------------------------------------------
//	The I2S clocks must have been started before performing this method.
//	This method sets the TAS3004 analog control register to normal operating
//	mode and unmutes the amplifers.  [2855519]  When waking the device it
//	is necessary to release at least one of the headphone or speaker amplifier
//	mute signals to release the Codec RESET on systems that implement the
//	Codec RESET by ANDing the headphone and speaker amplifier mute active states.
//	This must be done AFTER waking the I2S clocks!
IOReturn AppleTAS3004Audio::performDeviceWake () {
//	IOService *							keyLargo;
	IOReturn							err;

	debugIOLog ("+ AppleTAS3004Audio::performDeviceWake\n");

	err = kIOReturnSuccess;

	//	Set the TAS3004 analog control register to analog power up mode
	SetAnalogPowerDownMode (kPowerNormalAnalog);
    
	// ...then bring everything back up the way it should be.
	err = CODEC_Initialize ();			//	reset the TAS3001C and flush the shadow contents to the HW

	//	Mute the amplifiers as needed

	debugIOLog ( "- AppleTAS3004Audio::performDeviceWake\n" );
	return err;
}

#pragma mark + HARDWARE MANIPULATION
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Setup the pointer to the shadow register and the size of the shadow
//	register.
IOReturn 	AppleTAS3004Audio::GetShadowRegisterInfo( UInt8 regAddr, UInt8 ** shadowPtr, UInt8* registerSize ) {
	IOReturn		err;
	
	err = kIOReturnSuccess;
	FailWithAction( NULL == shadowPtr, err = kIOReturnError, Exit );
	FailWithAction( NULL == registerSize, err = kIOReturnError, Exit );
	
	switch( regAddr )
	{
		case kTAS3004MainCtrl1Reg:					*shadowPtr = (UInt8*)shadowTAS3004Regs.sMC1R;	*registerSize = kTAS3004MC1Rwidth;					break;
		case kTAS3004DynamicRangeCtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sDRC;	*registerSize = kTAS3004DRCwidth;					break;
		case kTAS3004VolumeCtrlReg:					*shadowPtr = (UInt8*)shadowTAS3004Regs.sVOL;	*registerSize = kTAS3004VOLwidth;					break;
		case kTAS3004TrebleCtrlReg:					*shadowPtr = (UInt8*)shadowTAS3004Regs.sTRE;	*registerSize = kTAS3004TREwidth;					break;
		case kTAS3004BassCtrlReg:					*shadowPtr = (UInt8*)shadowTAS3004Regs.sBAS;	*registerSize = kTAS3004BASwidth;					break;
		case kTAS3004MixerLeftGainReg:				*shadowPtr = (UInt8*)shadowTAS3004Regs.sMXL;	*registerSize = kTAS3004MIXERGAINwidth;				break;
		case kTAS3004MixerRightGainReg:				*shadowPtr = (UInt8*)shadowTAS3004Regs.sMXR;	*registerSize = kTAS3004MIXERGAINwidth;				break;
		case kTAS3004LeftBiquad0CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLB0;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftBiquad1CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLB1;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftBiquad2CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLB2;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftBiquad3CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLB3;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftBiquad4CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLB4;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftBiquad5CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLB5;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftBiquad6CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLB6;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightBiquad0CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sRB0;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightBiquad1CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sRB1;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightBiquad2CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sRB2;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightBiquad3CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sRB3;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightBiquad4CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sRB4;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightBiquad5CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sRB5;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightBiquad6CtrlReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sRB6;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftLoudnessBiquadReg:			*shadowPtr = (UInt8*)shadowTAS3004Regs.sLLB;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004RightLoudnessBiquadReg:		*shadowPtr = (UInt8*)shadowTAS3004Regs.sRLB;	*registerSize = kTAS3004BIQwidth;					break;
		case kTAS3004LeftLoudnessBiquadGainReg:		*shadowPtr = (UInt8*)shadowTAS3004Regs.sLLBG;	*registerSize = kTAS3004LOUDNESSBIQUADGAINwidth;	break;
		case kTAS3004RightLoudnessBiquadGainReg:	*shadowPtr = (UInt8*)shadowTAS3004Regs.sRLBG;	*registerSize = kTAS3004LOUDNESSBIQUADGAINwidth;	break;
		case kTAS3004AnalogControlReg:				*shadowPtr = (UInt8*)shadowTAS3004Regs.sACR;	*registerSize = kTAS3004ANALOGCTRLREGwidth;			break;
		case kTAS3004MainCtrl2Reg:					*shadowPtr = (UInt8*)shadowTAS3004Regs.sMC2R;	*registerSize = kTAS3004MC2Rwidth;					break;
		default:									err = kIOReturnError; /* notEnoughHardware  */														break;
	}
	
Exit:
    return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn AppleTAS3004Audio::InitEQSerialMode (UInt32 mode, Boolean restoreOnNormal)
{
	IOReturn		err;
	UInt8			initData;
	UInt8			previousData;
	
	debug3IOLog ("+ InitEQSerialMode (%8lX, %d)\n", mode, restoreOnNormal);
	initData = (kNormalLoad << kFL);
	if (kSetFastLoadMode == mode)
		initData = (kFastLoad << kFL);
		
	err = CODEC_ReadRegister (kTAS3004MainCtrl1Reg, &previousData);
	initData |= ( previousData & ~( 1 << kFL ) );
	err = CODEC_WriteRegister (kTAS3004MainCtrl1Reg, &initData, kFORCE_UPDATE_ALL);

	debug4IOLog ("- InitEQSerialMode (%8lX, %d) err = %d\n", mode, restoreOnNormal, err);
	return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn AppleTAS3004Audio::SetVolumeCoefficients( UInt32 left, UInt32 right )
{
	UInt8		volumeData[kTAS3004VOLwidth];
	IOReturn	err;
	
	debug3IOLog("SetVolumeCoefficients: L=%lX R=%lX\n", left, right);
	
	volumeData[2] = left;														
	volumeData[1] = left >> 8;												
	volumeData[0] = left >> 16;												
	
	volumeData[5] = right;														
	volumeData[4] = right >> 8;												
	volumeData[3] = right >> 16;
	
	err = CODEC_WriteRegister( kTAS3004VolumeCtrlReg, volumeData, kUPDATE_ALL );
	return err;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	This routine will perform a reset of the TAS3004C and then initialize
//	all registers within the TAS3004C to the values already held within 
//	the shadow registers.  The RESET sequence must not be performed until
//	the I2S clocks are running.	 The TAS3004C may hold the I2C bus signals
//	SDA and SCL low until the reset sequence (high->low->high) has been
//	completed.
IOReturn	AppleTAS3004Audio::CODEC_Initialize() {
	IOReturn	err;
	UInt32		retryCount;
	UInt32		initMode;
	UInt8		oldMode;
	UInt8		*shadowPtr;
	UInt8		registerSize;
	Boolean		done;
	
	debugIOLog ( "+ AppleTAS3004Audio::CODEC_Initialize\n" );

	err = kIOReturnError;
	done = false;
	oldMode = 0;
	initMode = kUPDATE_HW;
	retryCount = 0;
	if (!semaphores)
	{
		semaphores = 1;
		do{
//			if ( 0 != retryCount ) { debug2IOLog( "[AppleTAS3004Audio] ... RETRYING, retryCount %ld\n", retryCount ); }
			
            CODEC_Reset();

			if( 0 == oldMode )
				CODEC_ReadRegister( kTAS3004MainCtrl1Reg, &oldMode );					//	save previous load mode

			err = InitEQSerialMode( kSetFastLoadMode, kDontRestoreOnNormal );			//	set fast load mode for biquad initialization
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004LeftBiquad0CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftBiquad0CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004LeftBiquad1CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftBiquad1CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004LeftBiquad2CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftBiquad2CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004LeftBiquad3CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftBiquad3CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004LeftBiquad4CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftBiquad4CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004LeftBiquad5CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftBiquad5CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004LeftBiquad6CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftBiquad6CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004LeftLoudnessBiquadReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftLoudnessBiquadReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004RightBiquad0CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightBiquad0CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004RightBiquad1CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightBiquad1CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004RightBiquad2CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightBiquad2CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004RightBiquad3CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightBiquad3CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004RightBiquad4CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightBiquad4CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004RightBiquad5CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightBiquad5CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004RightBiquad6CtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightBiquad6CtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004RightLoudnessBiquadReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightLoudnessBiquadReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			err = InitEQSerialMode( kSetNormalLoadMode, kDontRestoreOnNormal );								//	set normal load mode for most register initialization
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004DynamicRangeCtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004DynamicRangeCtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
				
			GetShadowRegisterInfo( kTAS3004VolumeCtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004VolumeCtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004TrebleCtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004TrebleCtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004BassCtrlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004BassCtrlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004MixerLeftGainReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004MixerLeftGainReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
	
			GetShadowRegisterInfo( kTAS3004MixerRightGainReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004MixerRightGainReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );

			GetShadowRegisterInfo( kTAS3004LeftLoudnessBiquadGainReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004LeftLoudnessBiquadGainReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004RightLoudnessBiquadGainReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004RightLoudnessBiquadGainReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004AnalogControlReg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004AnalogControlReg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004MainCtrl2Reg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004MainCtrl2Reg, shadowPtr, initMode );
			FailIf( kIOReturnSuccess != err, AttemptToRetry );
			
			GetShadowRegisterInfo( kTAS3004MainCtrl1Reg, &shadowPtr, &registerSize );
			err = CODEC_WriteRegister( kTAS3004MainCtrl1Reg, &oldMode, initMode );			//	restore previous load mode
			FailIf( kIOReturnSuccess != err, AttemptToRetry );

AttemptToRetry:				
			if( kIOReturnSuccess == err )		//	terminate when successful
			{
				done = true;
			}
			retryCount++;
		} while ( !done && ( kTAS3004_MAX_RETRY_COUNT != retryCount ) );
		semaphores = 0;
		if( kTAS3004_MAX_RETRY_COUNT == retryCount )
			debug2IOLog( "\n\n\n\n          TAS3004 IS DEAD: Check %s\n\n\n\n", "ChooseAudio in fcr1" );
	}
	if( kIOReturnSuccess != err )
		debug2IOLog( "[AppleTAS3004Audio] CODEC_Initialize() err = %d\n", err );

//	debugIOLog ( "- AppleTAS3004Audio::CODEC_Initialize\n" );
    return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void	AppleTAS3004Audio::CODEC_LogRegisters () {
#if 0
//#if DEBUGLOG
	IOLog ( "TAS3004 MCR1:   %X\n", shadowTAS3004Regs.sMC1R[0] );
	IOLog ( "TAS3004 DRC:    %X %X %X %X %X %X\n", shadowTAS3004Regs.sDRC[0], shadowTAS3004Regs.sDRC[1], shadowTAS3004Regs.sDRC[2], shadowTAS3004Regs.sDRC[3], shadowTAS3004Regs.sDRC[4], shadowTAS3004Regs.sDRC[5] );
	IOLog ( "TAS3004 VOL:    %X%X%X %X%X%X\n", shadowTAS3004Regs.sVOL[0], shadowTAS3004Regs.sVOL[1], shadowTAS3004Regs.sVOL[2], shadowTAS3004Regs.sVOL[3], shadowTAS3004Regs.sVOL[4], shadowTAS3004Regs.sVOL[5] );
	IOLog ( "TAS3004 ACR:    %X\n", shadowTAS3004Regs.sACR[0] );
	IOLog ( "TAS3004 MCR2:   %X\n", shadowTAS3004Regs.sMC2R[0] );
	IOLog ( "TAS3004 MIXL:   %X%X%X %X%X%X %X%X%X\n", shadowTAS3004Regs.sMXL[0], shadowTAS3004Regs.sMXL[1], shadowTAS3004Regs.sMXL[2], shadowTAS3004Regs.sMXL[3], shadowTAS3004Regs.sMXL[4], shadowTAS3004Regs.sMXL[5], shadowTAS3004Regs.sMXL[6], shadowTAS3004Regs.sMXL[7], shadowTAS3004Regs.sMXL[8] );
	IOLog ( "TAS3004 MIXR:   %X%X%X %X%X%X %X%X%X\n", shadowTAS3004Regs.sMXR[0], shadowTAS3004Regs.sMXR[1], shadowTAS3004Regs.sMXR[2], shadowTAS3004Regs.sMXR[3], shadowTAS3004Regs.sMXR[4], shadowTAS3004Regs.sMXR[5], shadowTAS3004Regs.sMXR[6], shadowTAS3004Regs.sMXR[7], shadowTAS3004Regs.sMXR[8] );
#endif
}
	
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	[2855519]	Call down to lower level functions to implement the Codec
//	RESET assertion and negation where hardware dependencies exist...
void	AppleTAS3004Audio::CODEC_Reset ( void ) {

	mPlatformInterface->setCodecReset ( kCODEC_RESET_Analog, kGPIO_Run );
	IOSleep ( kCodec_RESET_SETUP_TIME );	//	I2S clocks must be running prerequisite to RESET
	mPlatformInterface->setCodecReset ( kCODEC_RESET_Analog, kGPIO_Reset );
	IOSleep ( kCodec_RESET_HOLD_TIME );
	mPlatformInterface->setCodecReset ( kCODEC_RESET_Analog, kGPIO_Run );
	IOSleep ( kCodec_RESET_RELEASE_TIME );	//	No I2C transactions for 

}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Reading registers with the TAS3004 is not possible.  A shadow register
//	is maintained for each TAS3004 hardware register.  Whenever a write
//	operation is performed on a hardware register, the data is written to 
//	the shadow register.  Read operations copy data from the shadow register
//	to the client register buffer.
IOReturn 	AppleTAS3004Audio::CODEC_ReadRegister(UInt8 regAddr, UInt8* registerData) {
	UInt8			registerSize;
	UInt32			regByteIndex;
	UInt8			*shadowPtr;
	IOReturn		err;
	
	err = kIOReturnSuccess;
	
	// quiet warnings caused by a complier that can't really figure out if something is going to be used uninitialized or not.
	registerSize = 0;
	shadowPtr = NULL;
	err = GetShadowRegisterInfo( regAddr, &shadowPtr, &registerSize );
	if( kIOReturnSuccess == err )
	{
		for( regByteIndex = 0; regByteIndex < registerSize; regByteIndex++ )
		{
			registerData[regByteIndex] = shadowPtr[regByteIndex];
		}
	}
	if( kIOReturnSuccess != err )
		debug4IOLog( "[AppleTAS3004Audio] %d notEnoughHardware = CODEC_ReadRegister( 0x%2.0X, 0x%8.0X )", err, regAddr, (unsigned int)registerData );

    return err;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	All TAS3004 write operations pass through this function so that a shadow
//	copy of the registers can be kept in global storage.  This function does enforce the data
//	size of the target register.  No partial register write operations are
//	supported.  IMPORTANT:  There is no enforcement regarding 'load' mode 
//	policy.  Client routines should properly maintain the 'load' mode by 
//	saving the contents of the master control register, set the appropriate 
//	load mode for the target register and then restore the previous 'load' 
//	mode.  All biquad registers should only be loaded while in 'fast load' 
//	mode.  All other registers should be loaded while in 'normal load' mode.
IOReturn 	AppleTAS3004Audio::CODEC_WriteRegister(UInt8 regAddr, UInt8* registerData, UInt8 mode){
	UInt8			registerSize;
	UInt32			regByteIndex;
	UInt8			*shadowPtr;
	IOReturn		err;
	Boolean			updateRequired;
	Boolean			success;
	
	err = kIOReturnSuccess;
	updateRequired = false;
	success = false;
	// quiet warnings caused by a complier that can't really figure out if something is going to be used uninitialized or not.
	registerSize = 0;
	shadowPtr = NULL;
	err = GetShadowRegisterInfo( regAddr, &shadowPtr, &registerSize );
	if( kIOReturnSuccess == err )
	{
		//	Write through to the shadow register as a 'write through' cache would and
		//	then write the data to the hardware;
		if( kUPDATE_SHADOW == mode || kUPDATE_ALL == mode || kFORCE_UPDATE_ALL == mode )
		{
			success = true;
			for( regByteIndex = 0; regByteIndex < registerSize; regByteIndex++ )
			{
//				if( shadowPtr[regByteIndex] != registerData[regByteIndex] && kUPDATE_ALL == mode )
//					updateRequired = true;
				shadowPtr[regByteIndex] = registerData[regByteIndex];
			}
		}
//		if( kUPDATE_HW == mode || updateRequired || kFORCE_UPDATE_ALL == mode )
		if( kUPDATE_HW == mode || kUPDATE_ALL == mode || kFORCE_UPDATE_ALL == mode )
		{
			success = mPlatformInterface->writeCodecRegister(kDEQAddress, regAddr, registerData, registerSize, kI2C_StandardSubMode);
			if ( !success ) {
				debug5IOLog ( "AppleTAS3004Audio::CODEC_WriteRegister mPlatformInterface->writeCodecRegister ( %x, %p, %x ) %d\n", regAddr, registerData, kI2C_StandardSubMode, success );
				mAudioDeviceProvider->interruptEventHandler ( kRequestCodecRecoveryStatus, (UInt32)kControlBusFatalErrorRecovery );
			}
		}
	}

	if( kIOReturnSuccess != err || !success ) {
		debug3IOLog ("error 0x%X returned, success == %d in AppleTAS3004Audio::CODEC_WriteRegister\n", err, success);
	}


    return err;
}


#pragma mark +UTILITY FUNCTIONS
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IORegistryEntry *AppleTAS3004Audio::FindEntryByNameAndProperty (const IORegistryEntry * start, const char * name, const char * key, UInt32 value) {
	OSIterator				*iterator;
	IORegistryEntry			*theEntry;
	IORegistryEntry			*tmpReg;
	OSNumber				*tmpNumber;

	theEntry = NULL;
	iterator = NULL;
	FailIf (NULL == start, Exit);

	iterator = start->getChildIterator (gIOServicePlane);
	FailIf (NULL == iterator, Exit);

	while (NULL == theEntry && (tmpReg = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != NULL) {
		if (strcmp (tmpReg->getName (), name) == 0) {
			tmpNumber = OSDynamicCast (OSNumber, tmpReg->getProperty (key));
			if (NULL != tmpNumber && tmpNumber->unsigned32BitValue () == value) {
				theEntry = tmpReg;
				theEntry->retain();
			}
		}
	}

Exit:
	if (NULL != iterator) {
		iterator->release ();
	}
	return theEntry;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IORegistryEntry *AppleTAS3004Audio::FindEntryByProperty (const IORegistryEntry * start, const char * key, const char * value) {
	OSIterator				*iterator;
	IORegistryEntry			*theEntry;
	IORegistryEntry			*tmpReg;
	OSData					*tmpData;

	theEntry = NULL;
	iterator = start->getChildIterator (gIODTPlane);
	FailIf (NULL == iterator, Exit);

	while (NULL == theEntry && (tmpReg = OSDynamicCast (IORegistryEntry, iterator->getNextObject ())) != NULL) {
		tmpData = OSDynamicCast (OSData, tmpReg->getProperty (key));
		if (NULL != tmpData && tmpData->isEqualTo (value, strlen (value))) {
			theEntry = tmpReg;
		}
	}

Exit:
	if (NULL != iterator) {
		iterator->release ();
	}
	return theEntry;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn AppleTAS3004Audio::GetCustomEQCoefficients ( UInt32 inEQIndex, EQPrefsElementPtr * eqPrefs ) {
	IOReturn		result = kIOReturnError;
	
	debug3IOLog ( "+ GetCustomEQCoefficients ( 0x%lX, %p )\n", inEQIndex, eqPrefs );
	FailIf ( NULL == eqPrefs, Exit );
	if ( inEQIndex < theEQPrefs.eqCount ) {
		*eqPrefs = &(kEQPrefsPtr->eq[inEQIndex]);
		result = kIOReturnSuccess;
	}
Exit:
	debug4IOLog ( "- GetCustomEQCoefficients ( 0x%lX, %p ) returns %d\n", inEQIndex, eqPrefs, result );

	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Boolean AppleTAS3004Audio::HasInput (void) {
	IORegistryEntry			*sound;
	OSData					*tmpData;
	UInt32					*numInputs = NULL;
	Boolean					hasInput;

	debugIOLog ( "+ AppleTAS3004Audio::HasInput\n" );
	hasInput = false;

	sound = mAudioDeviceProvider->getParentEntry (gIOServicePlane);
	FailIf (!sound, Exit);

	tmpData = OSDynamicCast (OSData, sound->getProperty (kNumInputs));
	FailIf (!tmpData, Exit);
	numInputs = (UInt32*)tmpData->getBytesNoCopy ();
	if (*numInputs > 1) {
		hasInput = true;
	}
Exit:
	debug3IOLog ( "- AppleTAS3004Audio::HasInput returns %d, numInputs %ld\n", hasInput, NULL == numInputs ? 0 : *numInputs );
	return hasInput;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void AppleTAS3004Audio::setEQ ( UInt32 inEQIndex ) {
	UInt8				volumeData[kTAS3004VOLwidth];
	EQPrefsElementPtr	eqPrefs = NULL;

	debug2IOLog ( "+ AppleTAS3004Audio::setEQ (%ld)\n", inEQIndex);

	for ( UInt32 index = 0; index < kTAS3004VOLwidth; index++ ) { volumeData[index] = 0; }	//	[2965804] prepare to mute volume
	CODEC_WriteRegister( kTAS3004VolumeCtrlReg, volumeData, kUPDATE_HW );					//	[2965804] mute volume without overwriting cache
	IOSleep ( kMAX_VOLUME_RAMP_DELAY );														//	[2965804] delay to allow mute to occur
	if ( !mDisableLoadingEQFromFile ) {
		if ( kIOReturnSuccess == GetCustomEQCoefficients (inEQIndex, &eqPrefs) ) {
			SndHWSetOutputBiquadGroup (eqPrefs->filterCount, eqPrefs->filter[0].coefficient);
	
			DRCInfo				localDRC;
	
			localDRC.compressionRatioNumerator		= eqPrefs->drcCompressionRatioNumerator;
			localDRC.compressionRatioDenominator	= eqPrefs->drcCompressionRatioDenominator;
			localDRC.threshold						= eqPrefs->drcThreshold;
			localDRC.maximumVolume					= eqPrefs->drcMaximumVolume;
			localDRC.enable							= (Boolean)((UInt32)(eqPrefs->drcEnable));
	
			SndHWSetDRC ((DRCInfoPtr)&localDRC);
		}
	}
	CODEC_ReadRegister( kTAS3004VolumeCtrlReg, volumeData );				//	[2965804] get cached volume setting
	CODEC_WriteRegister( kTAS3004VolumeCtrlReg, volumeData, kUPDATE_HW );	//	[2965804] and restore volume setting

	minVolume = kMinimumVolume;
	maxVolume = kMaximumVolume + drc.maximumVolume;

	return;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn AppleTAS3004Audio::prepareForOutputChange (void) {
	IOReturn				err;
    
	err = SetMixerState ( kMix0dB );
    if ( kIOReturnSuccess == err ) {
        err = SetAnalogPowerDownMode ( kPowerNormalAnalog );
	}
	
	return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	[2855519]	Leave the headphone amplifier unmuted until the speaker
//	amplifier has been unmuted to avoid applying a Codec RESET.
IOReturn AppleTAS3004Audio::SetMixerState ( UInt32 mixerState )
{
    IOReturn	err;
    UInt8		mixerData[kTAS3004MIXERGAINwidth];
    
    err = CODEC_ReadRegister( kTAS3004MixerLeftGainReg, mixerData );
    if ( kIOReturnSuccess == err ) {
		switch ( mixerState ) {
			case kMix0dB:			mixerData[0] = 0x10;		break;
			case kMixMute:			mixerData[0] = 0x00;		break;
		}
		err = CODEC_WriteRegister ( kTAS3004MixerLeftGainReg, mixerData, kUPDATE_ALL );
		if ( kIOReturnSuccess == err ) {
			err = CODEC_WriteRegister ( kTAS3004MixerRightGainReg, mixerData, kUPDATE_ALL );
		}
    }
    return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	This routine will only restore unity gain all pass coefficients to the
//	biquad registers.  All other coefficients to be passed through exported
//	functions via the sound hardware plug-in manager libaray.
void AppleTAS3004Audio::disableEQ (void) {
	int				biquadRefnum;
	DRCInfo			localDRC;
	int				numBiquads;
	UInt8			mcr2Data[kTAS3004MC2Rwidth];

	debugIOLog ( "+ AppleTAS3004Audio::disableEQ\n" );
	
	//	Set fast load mode to pause the DSP so that as the filter
	//	coefficients are applied, the filter will not become unstable
	//	and result in output instability.
	mcr2Data[0] = 0;
	CODEC_ReadRegister( kTAS3004MainCtrl2Reg, mcr2Data );
	mcr2Data[0] &= ~( kFilter_MASK << kAP );
	mcr2Data[0] |= ( kAllPassFilter << kAP );
	CODEC_WriteRegister( kTAS3004MainCtrl2Reg, mcr2Data, kFORCE_UPDATE_ALL );

	numBiquads = kNumberOfTAS3004BiquadsPerChannel;
	
	//	Set the biquad coefficients in the shadow registers to 'unity all pass' so that
	//	any future attempt to set the biquads is applied to the hardware registers (i.e.
	//	make sure that the shadow register accurately reflects the current state so that
	//	 a data compare in the future does not cause a write operation to be bypassed).
	for (biquadRefnum = 0; biquadRefnum < numBiquads; biquadRefnum++) {
		CODEC_WriteRegister (kTAS3004LeftBiquad0CtrlReg  + biquadRefnum, (UInt8*)kBiquad0db, kUPDATE_ALL);
		CODEC_WriteRegister (kTAS3004RightBiquad0CtrlReg + biquadRefnum, (UInt8*)kBiquad0db, kUPDATE_ALL);
	}
	
	//	Need to restore volume & mixer control registers after going to fast load mode
	
	localDRC.compressionRatioNumerator		= kDrcRatioNumerator;
	localDRC.compressionRatioDenominator	= kDrcRationDenominator;
	localDRC.threshold						= kDrcUnityThresholdHW;
	localDRC.maximumVolume					= kDefaultMaximumVolume;
	localDRC.enable							= false;

	SndHWSetDRC (&localDRC);

	minVolume = kMinimumVolume;
	maxVolume = kMaximumVolume + drc.maximumVolume;

	debugIOLog ( "- AppleTAS3004Audio::disableEQ\n" );
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	When disabling Dynamic Range Compression, don't check the other elements
//	of the DRCInfo structure.  When enabling DRC, clip the compression threshold
//	to a valid range for the target hardware & validate that the compression
//	ratio is supported by the target hardware.	The maximumVolume argument
//	will dynamically apply the zero index reference point into the volume
//	gain translation table and will force an update of the volume registers.
IOReturn AppleTAS3004Audio::SndHWSetDRC( DRCInfoPtr theDRCSettings ) {
	IOReturn		err;
	UInt8			regData[kTAS3004DRCwidth];
	Boolean			enableUpdated;

	debugIOLog ( "+ SndHWSetDRC\n" );
	err = kIOReturnSuccess;
	FailWithAction( NULL == theDRCSettings, err = kIOReturnError, Exit );
	FailWithAction( kDrcRatioNumerator != theDRCSettings->compressionRatioNumerator, err = kIOReturnError, Exit );
	FailWithAction( kDrcRationDenominator != theDRCSettings->compressionRatioDenominator, err = kIOReturnError, Exit );
	
	enableUpdated = drc.enable != theDRCSettings->enable ? true : false ;
	drc.enable = theDRCSettings->enable;

	//	The TAS3004 DRC threshold has a range of 0.0 dB through -89.625 dB.  The lowest value
	//	is rounded down to -90.0 dB so that a generalized formula for calculating the hardware
	//	value can be used.  The hardware values decrement two counts for each 0.75 dB of
	//	threshold change toward greater attenuation (i.e. more negative) where a 0.0 dB setting
	//	translates to a hardware setting of #-17 (i.e. kDRCUnityThreshold).  Since the threshold
	//	is passed in as a dB X 1000 value, the threshold is divided by the step size X 1000 or
	//	750, then multiplied by the hardware decrement value of 2 and the total is subtracted
	//	from the unity threshold hardware setting.  Note that the -90.0 dB setting actually
	//	would result in a hardware setting of -89.625 dB as the hardware settings become
	//	non-linear at the very lowest value.
	
	regData[DRC_Threshold]		= (UInt8)(kDRCUnityThreshold + (kDRC_CountsPerStep * (theDRCSettings->threshold / kDRC_ThreholdStepSize)));
	regData[DRC_AboveThreshold]	= theDRCSettings->enable ? kDRCAboveThreshold3to1 : kDisableDRC ;
	regData[DRC_BelowThreshold]	= kDRCBelowThreshold1to1;
	regData[DRC_Integration]	= kDRCIntegrationThreshold;
	regData[DRC_Attack]			= kDRCAttachThreshold;
	regData[DRC_Decay]			= kDRCDecayThreshold;
	err = CODEC_WriteRegister( kTAS3004DynamicRangeCtrlReg, regData, kFORCE_UPDATE_ALL );

	//	The current volume setting needs to be scaled against the new range of volume 
	//	control and applied to the hardware.
	if( drc.maximumVolume != theDRCSettings->maximumVolume || enableUpdated ) {
		drc.maximumVolume = theDRCSettings->maximumVolume;
	}
	
	drc.compressionRatioNumerator		= theDRCSettings->compressionRatioNumerator;
	drc.compressionRatioDenominator		= theDRCSettings->compressionRatioDenominator;
	drc.threshold						= theDRCSettings->threshold;
	drc.maximumVolume					= theDRCSettings->maximumVolume;

Exit:
	debug2IOLog ( "- SndHWSetDRC err = %d\n", err );
	return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	NOTE:	This function does not utilize fast mode loading as to do so would
//	revert all biquad coefficients not addressed by this execution instance to
//	unity all pass.	 Expect DSP processing delay if this function is used.	It
//	is recommended that SndHWSetOutputBiquadGroup be used instead.  THIS FUNCTION
//	WILL NOT ENABLE THE FILTERS.  DO NOT EXPORT THIS INTERFACE!!!
IOReturn AppleTAS3004Audio::SndHWSetOutputBiquad( UInt32 streamID, UInt32 biquadRefNum, FourDotTwenty *biquadCoefficients )
{
	IOReturn		err;
	UInt32			coefficientIndex;
	UInt32			TAS3004BiquadIndex;
	UInt32			biquadGroupIndex;
	UInt8			TAS3004Biquad[kTAS3004CoefficientsPerBiquad * kTAS3004NumBiquads];
	
	err = kIOReturnSuccess;
	FailWithAction( kTAS3004MaxBiquadRefNum < biquadRefNum || NULL == biquadCoefficients, err = kIOReturnError, Exit );
	FailWithAction( kStreamStereo != streamID && kStreamFrontLeft != streamID && kStreamFrontRight != streamID, err = kIOReturnError, Exit );
	
	TAS3004BiquadIndex = 0;
	biquadGroupIndex = biquadRefNum * kTAS3004CoefficientsPerBiquad;
	if( kStreamFrontRight == streamID )
		biquadGroupIndex += kNumberOfTAS3004BiquadCoefficientsPerChannel;

	for( coefficientIndex = 0; coefficientIndex < kTAS3004CoefficientsPerBiquad; coefficientIndex++ )
	{
		TAS3004Biquad[TAS3004BiquadIndex++] = biquadCoefficients[coefficientIndex].integerAndFraction1;
		TAS3004Biquad[TAS3004BiquadIndex++] = biquadCoefficients[coefficientIndex].fraction2;
		TAS3004Biquad[TAS3004BiquadIndex++] = biquadCoefficients[coefficientIndex].fraction3;
	}
	
	err = SetOutputBiquadCoefficients( streamID, biquadRefNum, TAS3004Biquad );

Exit:
	return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn AppleTAS3004Audio::SndHWSetOutputBiquadGroup( UInt32 biquadFilterCount, FourDotTwenty *biquadCoefficients )
{
	UInt32			index;
	IOReturn		err;
	UInt8			mcr2Data[kTAS3004MC2Rwidth];
	

	FailWithAction( 0 == biquadFilterCount || NULL == biquadCoefficients, err = kIOReturnError, Exit );
	err = kIOReturnSuccess;
	
	CODEC_ReadRegister( kTAS3004MainCtrl2Reg, mcr2Data );			//	bypass the filter while loading coefficients
	mcr2Data[0] &= ~( kFilter_MASK << kAP );
	mcr2Data[0] |= ( kAllPassFilter << kAP );
	CODEC_WriteRegister( kTAS3004MainCtrl2Reg, mcr2Data, kUPDATE_ALL );

	InitEQSerialMode( kSetFastLoadMode, kDontRestoreOnNormal );		//	pause the DSP while loading coefficients
	
	index = 0;
	do {
		if( index >= ( biquadFilterCount / 2 ) ) {
			err = SndHWSetOutputBiquad( kStreamFrontRight, index - ( biquadFilterCount / 2 ), biquadCoefficients );
		} else {
			err = SndHWSetOutputBiquad( kStreamFrontLeft, index, biquadCoefficients );
		}
		index++;
		biquadCoefficients += kNumberOfCoefficientsPerBiquad;
	} while ( ( index < biquadFilterCount ) && ( kIOReturnSuccess == err ) );
	
	InitEQSerialMode( kSetNormalLoadMode, kRestoreOnNormal );		//	enable the DSP

	CODEC_ReadRegister( kTAS3004MainCtrl2Reg, mcr2Data );			//	enable the filters
	mcr2Data[0] &= ~( kFilter_MASK << kAP );
	mcr2Data[0] |= ( kNormalFilter << kAP );
	CODEC_WriteRegister( kTAS3004MainCtrl2Reg, mcr2Data, kUPDATE_ALL );
Exit:
	return err;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
IOReturn AppleTAS3004Audio::SetOutputBiquadCoefficients( UInt32 streamID, UInt32 biquadRefNum, UInt8 *biquadCoefficients )
{
	IOReturn			err;
	
	err = kIOReturnSuccess;
	FailWithAction ( kTAS3004MaxBiquadRefNum < biquadRefNum || NULL == biquadCoefficients, err = kIOReturnError, Exit );
	FailWithAction ( kStreamStereo != streamID && kStreamFrontLeft != streamID && kStreamFrontRight != streamID, err = kIOReturnError, Exit );

	switch ( biquadRefNum )
	{
		case kBiquadRefNum_0:
			switch( streamID )
			{
				case kStreamFrontLeft:	err = CODEC_WriteRegister( kTAS3004LeftBiquad0CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamFrontRight:	err = CODEC_WriteRegister( kTAS3004RightBiquad0CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamStereo:		err = CODEC_WriteRegister( kTAS3004LeftBiquad0CtrlReg, biquadCoefficients, kUPDATE_ALL );
										err = CODEC_WriteRegister( kTAS3004RightBiquad0CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
			}
			break;
		case kBiquadRefNum_1:
			switch( streamID )
			{
				case kStreamFrontLeft:	err = CODEC_WriteRegister( kTAS3004LeftBiquad1CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamFrontRight:	err = CODEC_WriteRegister( kTAS3004RightBiquad1CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamStereo:		err = CODEC_WriteRegister( kTAS3004LeftBiquad1CtrlReg, biquadCoefficients, kUPDATE_ALL );
										err = CODEC_WriteRegister( kTAS3004RightBiquad1CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
			}
			break;
		case kBiquadRefNum_2:
			switch( streamID )
			{
				case kStreamFrontLeft:	err = CODEC_WriteRegister( kTAS3004LeftBiquad2CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamFrontRight:	err = CODEC_WriteRegister( kTAS3004RightBiquad2CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamStereo:		err = CODEC_WriteRegister( kTAS3004LeftBiquad2CtrlReg, biquadCoefficients, kUPDATE_ALL );
										err = CODEC_WriteRegister( kTAS3004RightBiquad2CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
			}
			break;
		case kBiquadRefNum_3:
			switch( streamID )
			{
				case kStreamFrontLeft:	err = CODEC_WriteRegister( kTAS3004LeftBiquad3CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamFrontRight:	err = CODEC_WriteRegister( kTAS3004RightBiquad3CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamStereo:		err = CODEC_WriteRegister( kTAS3004LeftBiquad3CtrlReg, biquadCoefficients, kUPDATE_ALL );
										err = CODEC_WriteRegister( kTAS3004RightBiquad3CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
			}
			break;
		case kBiquadRefNum_4:
			switch( streamID )
			{
				case kStreamFrontLeft:	err = CODEC_WriteRegister( kTAS3004LeftBiquad4CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamFrontRight:	err = CODEC_WriteRegister( kTAS3004RightBiquad4CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamStereo:		err = CODEC_WriteRegister( kTAS3004LeftBiquad4CtrlReg, biquadCoefficients, kUPDATE_ALL );
										err = CODEC_WriteRegister( kTAS3004RightBiquad4CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
			}
			break;
		case kBiquadRefNum_5:
			switch( streamID )
			{
				case kStreamFrontLeft:	err = CODEC_WriteRegister( kTAS3004LeftBiquad5CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamFrontRight:	err = CODEC_WriteRegister( kTAS3004RightBiquad5CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamStereo:		err = CODEC_WriteRegister( kTAS3004LeftBiquad5CtrlReg, biquadCoefficients, kUPDATE_ALL );
										err = CODEC_WriteRegister( kTAS3004RightBiquad5CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
			}
			break;
		case kBiquadRefNum_6:
			switch( streamID )
			{
				case kStreamFrontLeft:	err = CODEC_WriteRegister( kTAS3004LeftBiquad6CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamFrontRight:	err = CODEC_WriteRegister( kTAS3004RightBiquad6CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
				case kStreamStereo:		err = CODEC_WriteRegister( kTAS3004LeftBiquad6CtrlReg, biquadCoefficients, kUPDATE_ALL );
										err = CODEC_WriteRegister( kTAS3004RightBiquad6CtrlReg, biquadCoefficients, kUPDATE_ALL );	break;
			}
			break;
	}

Exit:
	return err;
}