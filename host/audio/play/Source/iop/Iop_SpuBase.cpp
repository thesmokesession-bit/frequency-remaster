#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <algorithm>
#include "string_format.h"
#include "Log.h"
#include "../states/RegisterStateCollectionFile.h"
#include "../states/RegisterStateUtils.h"
#include "../states/RegisterStateFile.h"
#include "Iop_SpuBase.h"

using namespace Iop;

#define INIT_SAMPLE_RATE (44100)
#define PITCH_BASE (0x1000)
#define TIME_SCALE (0x1000)
#define RESET_IRQ_ADDR (~0U)
#define LOG_NAME ("iop_spubase")

#define STATE_REGS_PATH_FORMAT ("iop_spu/spu_%d.xml")

#define STATE_REGS ("GlobalRegs")
#define STATE_REGS_CTRL ("CTRL")
#define STATE_REGS_IRQADDR ("IRQADDR")
#define STATE_REGS_IRQPENDING ("IRQPENDING")
#define STATE_REGS_TRANSFERADDR ("TRANSFERADDR")
#define STATE_REGS_TRANSFERMODE ("TRANSFERMODE")
#define STATE_REGS_CORE0OUTPUTOFFSET ("CORE0OUTPUTOFFSET")
#define STATE_REGS_CHANNELON ("CHANNELON")
#define STATE_REGS_CHANNELREVERB ("CHANNELREVERB")
#define STATE_REGS_REVERBWORKADDRSTART ("REVERBWORKADDRSTART")
#define STATE_REGS_REVERBWORKADDREND ("REVERBWORKADDREND")
#define STATE_REGS_REVERBCURRADDR ("REVERBCURRADDR")
#define STATE_REGS_REVERB_FORMAT ("REVERB%d")
#define STATE_REGS_INPVOLUMELEFT ("INPVOLUMELEFT")
#define STATE_REGS_INPVOLUMERIGHT ("INPVOLUMERIGHT")
#define STATE_REGS_EXTINPVOLUMELEFT ("EXTINPVOLUMELEFT")
#define STATE_REGS_EXTINPVOLUMERIGHT ("EXTINPVOLUMERIGHT")

#define STATE_CHANNEL_REGS_FORMAT ("Channel%02dRegs")
#define STATE_CHANNEL_REGS_VOLUMELEFT ("VOLUMELEFT")
#define STATE_CHANNEL_REGS_VOLUMERIGHT ("VOLUMERIGHT")
#define STATE_CHANNEL_REGS_VOLUMELEFTABS ("VOLUMELEFTABS")
#define STATE_CHANNEL_REGS_VOLUMERIGHTABS ("VOLUMERIGHTABS")
#define STATE_CHANNEL_REGS_STATUS ("STATUS")
#define STATE_CHANNEL_REGS_PITCH ("PITCH")
#define STATE_CHANNEL_REGS_ADSRLEVEL ("ADSRLEVEL")
#define STATE_CHANNEL_REGS_ADSRRATE ("ADSRRATE")
#define STATE_CHANNEL_REGS_ADSRVOLUME ("ADSRVOLUME")
#define STATE_CHANNEL_REGS_ADDRESS ("ADDRESS")
#define STATE_CHANNEL_REGS_REPEAT ("REPEAT")
#define STATE_CHANNEL_REGS_REPEATSET ("REPEATSET")
#define STATE_CHANNEL_REGS_CURRENT ("CURRENT")

#define STATE_SAMPLEREADER_REGS_SRCSAMPLEIDX ("SR_SrcSampleIdx")
#define STATE_SAMPLEREADER_REGS_SRCSAMPLINGRATE ("SR_SrcSamplingRate")
#define STATE_SAMPLEREADER_REGS_NEXTSAMPLEADDR ("SR_NextSampleAddr")
#define STATE_SAMPLEREADER_REGS_REPEATADDR ("SR_RepeatAddr")
#define STATE_SAMPLEREADER_REGS_PITCH ("SR_Pitch")
#define STATE_SAMPLEREADER_REGS_S1 ("SR_S1")
#define STATE_SAMPLEREADER_REGS_S2 ("SR_S2")
#define STATE_SAMPLEREADER_REGS_DONE ("SR_Done")
#define STATE_SAMPLEREADER_REGS_NEXTVALID ("SR_NextValid")
#define STATE_SAMPLEREADER_REGS_ENDFLAG ("SR_EndFlag")
#define STATE_SAMPLEREADER_REGS_DIDCHANGEREPEAT ("SR_DidChangeRepeat")
#define STATE_SAMPLEREADER_REGS_BUFFER_FORMAT ("SR_Buffer%d")

#define STATE_IRQWATCHER_REGS_PATH ("iop_spu/spu_irqwatcher.xml")

#define STATE_IRQWATCHER_REGS_IRQADDR0 ("irqAddr0")
#define STATE_IRQWATCHER_REGS_IRQADDR1 ("irqAddr1")
#define STATE_IRQWATCHER_REGS_IRQPENDING0 ("irqPending0")
#define STATE_IRQWATCHER_REGS_IRQPENDING1 ("irqPending1")

// clang-format off
bool CSpuBase::g_reverbParamIsAddress[REVERB_PARAM_COUNT] =
{
	true,
	true,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	false,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	true,
	false,
	false
};

const uint32 CSpuBase::g_linearIncreaseSweepDeltas[0x80] =
{
	0x3A0CC55E, 0x305FF9CE, 0x2976D61E, 0x203FFBDE, 0x1D0662AF, 0x182FFCE7, 0x1359971F, 0x101FFDEF,
	0x0DD2475F, 0x0C17FE73, 0x0A0233AF, 0x080FFEF7, 0x07144A05, 0x060BFF39, 0x050119D7, 0x03F9DC6C,
	0x037F3A27, 0x02FE04E5, 0x026B32E3, 0x01EF5BE9, 0x01B514DD, 0x018712AA, 0x01430F6B, 0x0100385E,
	0x00E129C7, 0x00BE85CF, 0x00A187B5, 0x00801C2F, 0x007094E3, 0x00607F9E, 0x004FE588, 0x003DEB7D,
	0x00392824, 0x00318930, 0x00271B77, 0x00204E57, 0x001B851B, 0x0017F80F, 0x00141506, 0x0010272B,
	0x000E0504, 0x000BFC07, 0x000A0A83, 0x0007FD5A, 0x0006C140, 0x00063126, 0x0004F41E, 0x0003E925,
	0x000389CC, 0x0002F8DF, 0x00027A0F, 0x0002021A, 0x0001C4E6, 0x00017C6F, 0x00014267, 0x0001010D,
	0x0000DFC9, 0x0000C023, 0x00009E83, 0x00007ECF, 0x00006FE4, 0x00005F1B, 0x00004F41, 0x00003F67,
	0x000037F2, 0x00002F8D, 0x000027A0, 0x0000203D, 0x00001BF9, 0x00001814, 0x00001405, 0x00000FD9,
	0x00000D96, 0x00000BE3, 0x00000A02, 0x000007EC, 0x0000070B, 0x000005F1, 0x00000501, 0x000003F6,
	0x00000385, 0x00000304, 0x00000280, 0x00000200, 0x000001BE, 0x0000017F, 0x00000140, 0x00000100,
	0x000000DF, 0x000000BF, 0x000000A0, 0x00000080, 0x0000006F, 0x0000005F, 0x00000050, 0x00000040,
	0x00000037, 0x0000002F, 0x00000028, 0x00000020, 0x0000001B, 0x00000017, 0x00000014, 0x00000010,
	0x0000000D, 0x0000000B, 0x0000000A, 0x00000008, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
};

const uint32 CSpuBase::g_linearDecreaseSweepDeltas[0x80] =
{
	0x488FF6B5, 0x3A0CC55E, 0x305FF9CE, 0x2976D61E, 0x203FFBDE, 0x1D0662AF, 0x182FFCE7, 0x1359971F,
	0x101FFDEF, 0x0DD2475F, 0x0C17FE73, 0x0A0233AF, 0x080FFEF7, 0x07144A05, 0x060BFF39, 0x050119D7,
	0x03F9DC6C, 0x037F3A27, 0x02FE04E5, 0x026B32E3, 0x01EF5BE9, 0x01B514DD, 0x018712AA, 0x01430F6B,
	0x0100385E, 0x00E129C7, 0x00BE85CF, 0x00A187B5, 0x00801C2F, 0x007094E3, 0x00607F9E, 0x004FE588,
	0x003DEB7D, 0x00392824, 0x00318930, 0x00271B77, 0x00204E57, 0x001B851B, 0x0017F80F, 0x00141506,
	0x0010272B, 0x000E0504, 0x000BFC07, 0x000A0A83, 0x0007FD5A, 0x0006C140, 0x00063126, 0x0004F41E,
	0x0003E925, 0x000389CC, 0x0002F8DF, 0x00027A0F, 0x0002021A, 0x0001C4E6, 0x00017C6F, 0x00014267,
	0x0001010D, 0x0000DFC9, 0x0000C023, 0x00009E83, 0x00007ECF, 0x00006FE4, 0x00005F1B, 0x00004F41,
	0x00003F67, 0x000037F2, 0x00002F8D, 0x000027A0, 0x0000203D, 0x00001BF9, 0x00001814, 0x00001405,
	0x00000FD9, 0x00000D96, 0x00000BE3, 0x00000A02, 0x000007EC, 0x0000070B, 0x000005F1, 0x00000501,
	0x000003F6, 0x00000385, 0x00000304, 0x00000280, 0x00000200, 0x000001BE, 0x0000017F, 0x00000140,
	0x00000100, 0x000000DF, 0x000000BF, 0x000000A0, 0x00000080, 0x0000006F, 0x0000005F, 0x00000050,
	0x00000040, 0x00000037, 0x0000002F, 0x00000028, 0x00000020, 0x0000001B, 0x00000017, 0x00000014,
	0x00000010, 0x0000000D, 0x0000000B, 0x0000000A, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
	0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000
};
// clang-format on

CSpuBase::CSpuBase(uint8* ram, uint32 ramSize, CSpuSampleCache* sampleCache, CSpuIrqWatcher* irqWatcher, unsigned int spuNumber)
    : m_ram(ram)
    , m_ramSize(ramSize)
    , m_spuNumber(spuNumber)
    , m_sampleCache(sampleCache)
    , m_irqWatcher(irqWatcher)
    , m_reverbEnabled(true)
{
	Reset();

	//Init log table for ADSR
	memset(m_adsrLogTable, 0, sizeof(m_adsrLogTable));

	uint32 value = 3;
	uint32 columnIncrement = 1;
	uint32 column = 0;

	for(unsigned int i = 32; i < 160; i++)
	{
		if(value < 0x3FFFFFFF)
		{
			value += columnIncrement;
			column++;
			if(column == 5)
			{
				column = 1;
				columnIncrement *= 2;
			}
		}
		else
		{
			value = 0x3FFFFFFF;
		}
		m_adsrLogTable[i] = value;
	}
}

void CSpuBase::Reset()
{
	m_ctrl = 0;

	m_volumeAdjust = 1.0f;

	m_channelOn.f = 0;
	m_channelReverb.f = 0;
	m_reverbTicks = 0;
	m_irqAddr = RESET_IRQ_ADDR;
	m_irqPending = false;
	m_transferMode = 0;
	m_transferAddr = 0;

	m_core0OutputOffset = 0;

	m_reverbCurrAddr = 0;
	m_reverbWorkAddrStart = 0;
	m_reverbWorkAddrEnd = 0x80000;
	m_baseSamplingRate = INIT_SAMPLE_RATE;

	m_inputVolL = 0x7FFF;
	m_inputVolR = 0x7FFF;
	m_extInputVolL = 0x7FFF;
	m_extInputVolR = 0x7FFF;
	m_reverbOutVolL = 0;
	m_reverbOutVolR = 0;
	m_reverbIn[0] = m_reverbIn[1] = 0;
	m_reverbOut[0] = m_reverbOut[1] = 0;

	memset(m_channel, 0, sizeof(m_channel));
	memset(m_reverb, 0, sizeof(m_reverb));

	for(unsigned int i = 0; i < MAX_CHANNEL; i++)
	{
		auto& reader = m_reader[i];
		reader.Reset();
		reader.SetMemory(m_ram, m_ramSize);
		reader.SetSampleCache(m_sampleCache);
		reader.SetIrqWatcher(m_irqWatcher);
	}

	m_blockReader.Reset();
	m_soundInputDataAddr = (m_spuNumber == 0) ? SOUND_INPUT_DATA_CORE0_BASE : SOUND_INPUT_DATA_CORE1_BASE;
	m_blockWritePtr = 0;
}

void CSpuBase::LoadState(Framework::CZipArchiveReader& archive)
{
	auto path = string_format(STATE_REGS_PATH_FORMAT, m_spuNumber);
	auto stateCollectionFile = CRegisterStateCollectionFile(*archive.BeginReadFile(path.c_str()));

	{
		const auto& state = stateCollectionFile.GetRegisterState(STATE_REGS);
		m_ctrl = state.GetRegister32(STATE_REGS_CTRL);
		m_irqAddr = state.GetRegister32(STATE_REGS_IRQADDR);
		m_irqPending = state.GetRegister32(STATE_REGS_IRQPENDING) != 0;
		m_transferMode = state.GetRegister32(STATE_REGS_TRANSFERMODE);
		m_transferAddr = state.GetRegister32(STATE_REGS_TRANSFERADDR);
		m_core0OutputOffset = state.GetRegister32(STATE_REGS_CORE0OUTPUTOFFSET);
		m_channelOn.f = state.GetRegister32(STATE_REGS_CHANNELON);
		m_channelReverb.f = state.GetRegister32(STATE_REGS_CHANNELREVERB);
		m_reverbWorkAddrStart = state.GetRegister32(STATE_REGS_REVERBWORKADDRSTART);
		m_reverbWorkAddrEnd = state.GetRegister32(STATE_REGS_REVERBWORKADDREND);
		m_reverbCurrAddr = state.GetRegister32(STATE_REGS_REVERBCURRADDR);
		m_inputVolL = state.GetRegister32(STATE_REGS_INPVOLUMELEFT);
		m_inputVolR = state.GetRegister32(STATE_REGS_INPVOLUMERIGHT);
		m_extInputVolL = state.GetRegister32(STATE_REGS_EXTINPVOLUMELEFT);
		m_extInputVolR = state.GetRegister32(STATE_REGS_EXTINPVOLUMERIGHT);
		RegisterStateUtils::ReadArray(state, m_reverb, STATE_REGS_REVERB_FORMAT);
	}

	for(unsigned int i = 0; i < MAX_CHANNEL; i++)
	{
		auto& channel = m_channel[i];
		auto& reader = m_reader[i];

		auto channelRegsName = string_format(STATE_CHANNEL_REGS_FORMAT, i);
		const auto& channelState = stateCollectionFile.GetRegisterState(channelRegsName.c_str());

		channel.volumeLeft <<= channelState.GetRegister32(STATE_CHANNEL_REGS_VOLUMELEFT);
		channel.volumeRight <<= channelState.GetRegister32(STATE_CHANNEL_REGS_VOLUMERIGHT);
		channel.volumeLeftAbs = channelState.GetRegister32(STATE_CHANNEL_REGS_VOLUMELEFTABS);
		channel.volumeRightAbs = channelState.GetRegister32(STATE_CHANNEL_REGS_VOLUMERIGHTABS);
		channel.status = channelState.GetRegister32(STATE_CHANNEL_REGS_STATUS);
		channel.pitch = channelState.GetRegister32(STATE_CHANNEL_REGS_PITCH);
		channel.adsrLevel <<= channelState.GetRegister32(STATE_CHANNEL_REGS_ADSRLEVEL);
		channel.adsrRate <<= channelState.GetRegister32(STATE_CHANNEL_REGS_ADSRRATE);
		channel.adsrVolume = channelState.GetRegister32(STATE_CHANNEL_REGS_ADSRVOLUME);
		channel.address = channelState.GetRegister32(STATE_CHANNEL_REGS_ADDRESS);
		channel.repeat = channelState.GetRegister32(STATE_CHANNEL_REGS_REPEAT);
		channel.repeatSet = channelState.GetRegister32(STATE_CHANNEL_REGS_REPEATSET) != 0;
		channel.current = channelState.GetRegister32(STATE_CHANNEL_REGS_CURRENT);
		reader.LoadState(channelState);
	}
}

void CSpuBase::SaveState(Framework::CZipArchiveWriter& archive)
{
	auto path = string_format(STATE_REGS_PATH_FORMAT, m_spuNumber);
	auto stateCollectionFile = std::make_unique<CRegisterStateCollectionFile>(path.c_str());

	{
		CRegisterState state;
		state.SetRegister32(STATE_REGS_CTRL, m_ctrl);
		state.SetRegister32(STATE_REGS_IRQADDR, m_irqAddr);
		state.SetRegister32(STATE_REGS_IRQPENDING, m_irqPending);
		state.SetRegister32(STATE_REGS_TRANSFERMODE, m_transferMode);
		state.SetRegister32(STATE_REGS_TRANSFERADDR, m_transferAddr);
		state.SetRegister32(STATE_REGS_CORE0OUTPUTOFFSET, m_core0OutputOffset);
		state.SetRegister32(STATE_REGS_CHANNELON, m_channelOn.f);
		state.SetRegister32(STATE_REGS_CHANNELREVERB, m_channelReverb.f);
		state.SetRegister32(STATE_REGS_REVERBWORKADDRSTART, m_reverbWorkAddrStart);
		state.SetRegister32(STATE_REGS_REVERBWORKADDREND, m_reverbWorkAddrEnd);
		state.SetRegister32(STATE_REGS_REVERBCURRADDR, m_reverbCurrAddr);
		state.SetRegister32(STATE_REGS_INPVOLUMELEFT, m_inputVolL);
		state.SetRegister32(STATE_REGS_INPVOLUMERIGHT, m_inputVolR);
		state.SetRegister32(STATE_REGS_EXTINPVOLUMELEFT, m_extInputVolL);
		state.SetRegister32(STATE_REGS_EXTINPVOLUMERIGHT, m_extInputVolR);
		RegisterStateUtils::WriteArray(state, m_reverb, STATE_REGS_REVERB_FORMAT);
		stateCollectionFile->InsertRegisterState(STATE_REGS, std::move(state));
	}

	for(unsigned int i = 0; i < MAX_CHANNEL; i++)
	{
		const auto& channel = m_channel[i];
		const auto& reader = m_reader[i];
		CRegisterState channelState;
		channelState.SetRegister32(STATE_CHANNEL_REGS_VOLUMELEFT, channel.volumeLeft);
		channelState.SetRegister32(STATE_CHANNEL_REGS_VOLUMERIGHT, channel.volumeRight);
		channelState.SetRegister32(STATE_CHANNEL_REGS_VOLUMELEFTABS, channel.volumeLeftAbs);
		channelState.SetRegister32(STATE_CHANNEL_REGS_VOLUMERIGHTABS, channel.volumeRightAbs);
		channelState.SetRegister32(STATE_CHANNEL_REGS_STATUS, channel.status);
		channelState.SetRegister32(STATE_CHANNEL_REGS_PITCH, channel.pitch);
		channelState.SetRegister32(STATE_CHANNEL_REGS_ADSRLEVEL, channel.adsrLevel);
		channelState.SetRegister32(STATE_CHANNEL_REGS_ADSRRATE, channel.adsrRate);
		channelState.SetRegister32(STATE_CHANNEL_REGS_ADSRVOLUME, channel.adsrVolume);
		channelState.SetRegister32(STATE_CHANNEL_REGS_ADDRESS, channel.address);
		channelState.SetRegister32(STATE_CHANNEL_REGS_REPEAT, channel.repeat);
		channelState.SetRegister32(STATE_CHANNEL_REGS_REPEATSET, channel.repeatSet);
		channelState.SetRegister32(STATE_CHANNEL_REGS_CURRENT, channel.current);
		reader.SaveState(channelState);

		auto channelRegsName = string_format(STATE_CHANNEL_REGS_FORMAT, i);
		stateCollectionFile->InsertRegisterState(channelRegsName.c_str(), std::move(channelState));
	}

	archive.InsertFile(std::move(stateCollectionFile));
}

bool CSpuBase::IsEnabled() const
{
	return (m_ctrl & 0x8000) != 0;
}

void CSpuBase::SetVolumeAdjust(float volumeAdjust)
{
	m_volumeAdjust = volumeAdjust;
}

void CSpuBase::SetReverbEnabled(bool enabled)
{
	m_reverbEnabled = enabled;
}

uint16 CSpuBase::GetControl() const
{
	return m_ctrl;
}

void CSpuBase::SetControl(uint16 value)
{
	m_ctrl = value;
	if((m_ctrl & CONTROL_IRQ) == 0)
	{
		ClearIrqPending();
		m_irqWatcher->ClearIrqPending(m_spuNumber);
	}
}

void CSpuBase::SetBaseSamplingRate(uint32 samplingRate)
{
	m_baseSamplingRate = samplingRate;
	m_blockReader.SetBaseSamplingRate(samplingRate);
}

void CSpuBase::SetInputBypass(bool inputBypass)
{
	m_blockReader.SetSpdifBypass(inputBypass);
}

void CSpuBase::SetDestinationSamplingRate(uint32 samplingRate)
{
	m_blockReader.SetDestinationSamplingRate(samplingRate);
	for(auto& reader : m_reader)
	{
		reader.SetDestinationSamplingRate(samplingRate);
	}
}

bool CSpuBase::GetIrqPending() const
{
	return m_irqPending;
}

void CSpuBase::ClearIrqPending()
{
	m_irqPending = false;
}

uint32 CSpuBase::GetIrqAddress() const
{
	return m_irqAddr;
}

void CSpuBase::SetIrqAddress(uint32 value)
{
	m_irqAddr = value & (m_ramSize - 1);
	m_irqWatcher->SetIrqAddress(m_spuNumber, m_irqAddr);
}

uint16 CSpuBase::GetTransferMode() const
{
	return m_transferMode;
}

void CSpuBase::SetTransferMode(uint16 transferMode)
{
	m_transferMode = transferMode;
}

uint32 CSpuBase::GetTransferAddress() const
{
	return m_transferAddr;
}

void CSpuBase::SetTransferAddress(uint32 value)
{
	m_transferAddr = value & (m_ramSize - 1);
}

UNION32_16 CSpuBase::GetChannelOn() const
{
	return m_channelOn;
}

void CSpuBase::SetChannelOnLo(uint16 value)
{
	m_channelOn.h0 = value;
}

void CSpuBase::SetChannelOnHi(uint16 value)
{
	m_channelOn.h1 = value;
}

UNION32_16 CSpuBase::GetChannelReverb() const
{
	return m_channelReverb;
}

void CSpuBase::SetChannelReverbLo(uint16 value)
{
	m_channelReverb.h0 = value;
}

void CSpuBase::SetChannelReverbHi(uint16 value)
{
	m_channelReverb.h1 = value;
}

uint32 CSpuBase::GetReverbParam(unsigned int param) const
{
	assert(param < REVERB_PARAM_COUNT);
	return m_reverb[param];
}

void CSpuBase::SetReverbParam(unsigned int param, uint32 value)
{
	assert(param < REVERB_PARAM_COUNT);
	m_reverb[param] = value;
}

UNION32_16 CSpuBase::GetEndFlags() const
{
	UNION32_16 result(0);
	for(unsigned int i = 0; i < MAX_CHANNEL; i++)
	{
		if(m_reader[i].GetEndFlag())
		{
			result.f |= (1 << i);
		}
	}
	return result;
}

void CSpuBase::ClearEndFlags()
{
	for(unsigned int i = 0; i < MAX_CHANNEL; i++)
	{
		m_reader[i].ClearEndFlag();
	}
}

CSpuBase::CHANNEL& CSpuBase::GetChannel(unsigned int channelNumber)
{
	assert(channelNumber < MAX_CHANNEL);
	return m_channel[channelNumber];
}

void CSpuBase::OnChannelPitchChanged(unsigned int channelNumber)
{
	assert(channelNumber < MAX_CHANNEL);
	auto& reader = m_reader[channelNumber];
	auto& channel = m_channel[channelNumber];
	reader.SetPitch(m_baseSamplingRate, channel.pitch);
}

void CSpuBase::SendKeyOn(uint32 channels)
{
	for(unsigned int i = 0; i < MAX_CHANNEL; i++)
	{
		CHANNEL& channel = m_channel[i];
		if(channels & (1 << i))
		{
			channel.status = KEY_ON;
			channel.repeatSet = false;
			channel.current = channel.address;
			//Simulate one tick of ADSR in case game tries to read envelope value quickly
			//(ie.: Before 'Render' is called and while the voice is still in KEY_ON state)
			channel.adsrVolume = GetAdsrDelta((channel.adsrLevel.attackRate ^ 0x7F) - 0x10);
		}
	}
}

void CSpuBase::SendKeyOff(uint32 channels)
{
	for(unsigned int i = 0; i < MAX_CHANNEL; i++)
	{
		CHANNEL& channel = m_channel[i];
		if(channels & (1 << i))
		{
			if(channel.status == STOPPED) continue;
			if(channel.status == KEY_ON)
			{
				channel.status = STOPPED;
				//Update channel registers since we are doing KEY_ON -> STOPPED transition
				auto& reader = m_reader[i];
				reader.SetParamsNoRead(channel.address, channel.repeat);
				reader.ClearEndFlag();
				channel.current = reader.GetCurrent();
			}
			else
			{
				channel.status = RELEASE;
			}
		}
	}
}

uint32 CSpuBase::GetReverbWorkAddressStart() const
{
	return m_reverbWorkAddrStart;
}

void CSpuBase::SetReverbWorkAddressStart(uint32 address)
{
	assert(address <= m_ramSize);
	m_reverbWorkAddrStart = address;
	m_reverbCurrAddr = address;
}

uint32 CSpuBase::GetReverbWorkAddressEnd() const
{
	return m_reverbWorkAddrEnd - 1;
}

void CSpuBase::SetReverbWorkAddressEnd(uint32 address)
{
	assert((address & 0xFFFF) == 0xFFFF);
	assert(address <= m_ramSize);
	m_reverbWorkAddrEnd = address + 1;
}

void CSpuBase::SetReverbCurrentAddress(uint32 address)
{
	m_reverbCurrAddr = address;
}

uint32 CSpuBase::ReceiveDma(uint8* buffer, uint32 blockSize, uint32 blockAmount, uint32 direction)
{
#ifdef _DEBUG
	CLog::GetInstance().Print(LOG_NAME, "Receiving DMA transfer to 0x%08X. Size = 0x%08X bytes.\r\n",
	                          m_transferAddr, blockSize * blockAmount);
#endif
	if(m_transferMode == TRANSFER_MODE_VOICE)
	{
		if((m_ctrl & CONTROL_DMA) == CONTROL_DMA_STOP)
		{
			//Genso Suikoden 5 uses this
			return 0;
		}
		if((m_ctrl & CONTROL_DMA) == CONTROL_DMA_READ)
		{
			//- DMA reads need to be throttled to allow FFX IopSoundDriver to properly synchronize itself
			blockAmount = std::min<uint32>(blockAmount, 0x10);
			for(unsigned int i = 0; i < blockAmount; i++)
			{
				memcpy(buffer, m_ram + m_transferAddr, blockSize);
				m_transferAddr += blockSize;
				m_transferAddr &= m_ramSize - 1;
				buffer += blockSize;
			}
			return blockAmount;
		}
		//- Tsugunai needs voice transfers to be throttled because it starts a DMA transfer
		//  and then writes data that is necessary to the transfer callback in memory
		//- Some PSF sets (FF4, Xenogears, Xenosaga 2) are sensitive to aggressive throttling (doesn't like 0x10)
		blockAmount = std::min<uint32>(blockAmount, 0x100);
		assert((m_ctrl & CONTROL_DMA) == CONTROL_DMA_WRITE);
		unsigned int blocksTransfered = 0;
		m_sampleCache->ClearRange(m_transferAddr, blockSize * blockAmount);
		for(unsigned int i = 0; i < blockAmount; i++)
		{
			uint32 copySize = std::min<uint32>(m_ramSize - m_transferAddr, blockSize);
			memcpy(m_ram + m_transferAddr, buffer, copySize);
			m_transferAddr += blockSize;
			m_transferAddr &= m_ramSize - 1;
			buffer += blockSize;
			blocksTransfered++;
		}
		return blocksTransfered;
	}
	else if(
	    (m_transferMode == TRANSFER_MODE_BLOCK_CORE0IN) ||
	    (m_transferMode == TRANSFER_MODE_BLOCK_CORE1IN))
	{
		assert(m_transferAddr == 0);
		assert((m_spuNumber == 0) || !(m_transferMode == TRANSFER_MODE_BLOCK_CORE0IN));
		assert((m_spuNumber == 1) || !(m_transferMode == TRANSFER_MODE_BLOCK_CORE1IN));
		assert(m_blockWritePtr <= SOUND_INPUT_DATA_SIZE);

		uint32 availableBytes = SOUND_INPUT_DATA_SIZE - m_blockWritePtr;
		uint32 availableBlocks = availableBytes / blockSize;
		blockAmount = std::min(blockAmount, availableBlocks);

		uint32 dstAddr = m_soundInputDataAddr + m_blockWritePtr;
		memcpy(m_ram + dstAddr, buffer, blockAmount * blockSize);
		m_blockWritePtr += blockAmount * blockSize;

		return blockAmount;
	}
	else
	{
		return 1;
	}
}

void CSpuBase::WriteWord(uint16 value)
{
	assert((m_transferAddr + 1) < m_ramSize);
	*reinterpret_cast<uint16*>(&m_ram[m_transferAddr]) = value;
	m_sampleCache->ClearRange(m_transferAddr, 2);
	m_transferAddr += 2;
}

int32 CSpuBase::ComputeChannelVolume(const CHANNEL_VOLUME& volume, int32 currentVolume)
{
	int32 volumeLevel = 0;
	if(!volume.mode.mode)
	{
		if(volume.volume.phase)
		{
			volumeLevel = 0x3FFF - volume.volume.volume;
		}
		else
		{
			volumeLevel = volume.volume.volume;
		}
		volumeLevel <<= 17;
	}
	else
	{
		assert(volume.sweep.phase == 0);
		if(volume.sweep.slope == 0)
		{
			//Linear increase/decrease
			if(volume.sweep.decrease)
			{
				uint32 sweepDelta = g_linearDecreaseSweepDeltas[volume.sweep.volume];
				volumeLevel = currentVolume - sweepDelta;
			}
			else
			{
				uint32 sweepDelta = g_linearIncreaseSweepDeltas[volume.sweep.volume];
				volumeLevel = currentVolume + sweepDelta;
			}
		}
		else
		{
			//Exponential increase/decrease
			if(volume.sweep.decrease)
			{
				int64 sweepDelta = static_cast<int64>(currentVolume) * static_cast<int64>(volume.sweep.volume) / 0x7F;
				assert(sweepDelta >= 0);
				int32 baseVolume = std::max(1, currentVolume);
				uint32 sweepDeltaClamped = std::clamp<int64>(sweepDelta, 1, baseVolume);
				volumeLevel = baseVolume - sweepDeltaClamped;
			}
			else
			{
				//Not supported
				assert(false);
			}
		}
		volumeLevel = std::max<int32>(volumeLevel, 0x00000000);
		volumeLevel = std::min<int32>(volumeLevel, 0x7FFFFFFF);
	}
	return volumeLevel;
}

void CSpuBase::MixSamples(int32 inputSample, int32 volumeLevel, int16* output)
{
	inputSample = (inputSample * volumeLevel) / 0x7FFF;
	int32 resultSample = inputSample + static_cast<int32>(*output);
	resultSample = std::clamp<int32>(resultSample, SHRT_MIN, SHRT_MAX);

	*output = static_cast<int16>(resultSample);
}

void CSpuBase::Render(int16* samples, unsigned int sampleCount)
{
	bool updateReverb = m_reverbEnabled && (m_ctrl & CONTROL_REVERB) && (m_reverbWorkAddrStart < m_reverbWorkAddrEnd);
	bool irqEnabled = (m_ctrl & CONTROL_IRQ);

	int16* samplesBase = samples;
	assert((sampleCount & 0x01) == 0);
	unsigned int ticks = sampleCount / 2;
	memset(samples, 0, sizeof(int16) * sampleCount);

	for(unsigned int j = 0; j < ticks; j++)
	{
		int16 reverbSample[2] = {};
		//Update channels
		for(unsigned int i = 0; i < 24; i++)
		{
			auto& channel(m_channel[i]);
			auto& reader(m_reader[i]);
			if(channel.status == KEY_ON)
			{
				reader.SetParamsRead(channel.address, channel.repeat);
				reader.ClearEndFlag();
				channel.status = ATTACK;
				channel.adsrVolume = 0;
			}
			else
			{
				if(reader.IsDone())
				{
					channel.status = STOPPED;
					channel.adsrVolume = 0;
					reader.ClearIsDone();
				}
				if(reader.DidChangeRepeat() && !channel.repeatSet)
				{
					channel.repeat = reader.GetRepeat();
					reader.ClearDidChangeRepeat();
				}
				//Update repeat in case it has been changed externally (needed for FFX)
				reader.SetRepeat(channel.repeat);
			}

			int32 readSample = reader.GetSample();
			channel.current = reader.GetCurrent();

			UpdateAdsr(channel);
			channel.volumeLeftAbs = ComputeChannelVolume(channel.volumeLeft, channel.volumeLeftAbs);
			channel.volumeRightAbs = ComputeChannelVolume(channel.volumeRight, channel.volumeRightAbs);

			if(readSample == 0) continue;

			//Mix in adsrVolume
			int32 inputSample = (readSample * static_cast<int32>(channel.adsrVolume >> 16)) / static_cast<int32>(MAX_ADSR_VOLUME >> 16);

			if(inputSample == 0) continue;

			int32 volumeLeft = channel.volumeLeftAbs >> 16;
			int32 volumeRight = channel.volumeRightAbs >> 16;

			MixSamples(inputSample, volumeLeft, samples + 0);
			MixSamples(inputSample, volumeRight, samples + 1);

			//Mix in reverb if enabled for this channel
			if(updateReverb && (m_channelReverb.f & (1 << i)))
			{
				MixSamples(inputSample, volumeLeft, reverbSample + 0);
				MixSamples(inputSample, volumeRight, reverbSample + 1);
			}
		}

		if(!m_blockReader.CanReadSamples() && (m_blockWritePtr == SOUND_INPUT_DATA_SIZE))
		{
			//We're ready to consume some data
			m_blockReader.FillBlock(m_ram + m_soundInputDataAddr);
			m_blockWritePtr = 0;
		}

		if(m_blockReader.CanReadSamples())
		{
			int32 blockSamples[2] = {};
			m_blockReader.GetSamples(blockSamples);

			// Audio input data should have volume adjusted to BVOL register values . . .
			if(m_spuNumber == 0 && m_blockReader.GetSpdifBypass())
			{
				//  . . . unless in bypass mode
				MixSamples(blockSamples[0], 0x7FFF, samples + 0);
				MixSamples(blockSamples[1], 0x7FFF, samples + 1);
			}
			else
			{
				MixSamples(blockSamples[0], m_inputVolL, samples + 0);
				MixSamples(blockSamples[1], m_inputVolR, samples + 1);
			}
		}

		//Simulate SPU CORE0 writing its output in RAM and check for potential interrupts
		if(m_spuNumber == 0)
		{
			if(irqEnabled)
			{
				//TODO: Check which core is responsible for which area
				if(m_irqAddr == (CORE0_SIN_LEFT + m_core0OutputOffset))
				{
					m_irqPending = true;
				}
				else if(m_irqAddr == (CORE1_SIN_LEFT + m_core0OutputOffset))
				{
					m_irqPending = true;
				}
				else if(m_irqAddr == (CORE1_SIN_RIGHT + m_core0OutputOffset))
				{
					m_irqPending = true;
				}
			}
			m_core0OutputOffset += 2;
			m_core0OutputOffset &= (CORE0_OUTPUT_SIZE - 1);
		}

		//Update reverb
		if(updateReverb)
		{
			UpdateReverb(reverbSample, samples);
		}

		samples += 2;
	}

	if(irqEnabled && m_irqWatcher->HasPendingIrq(m_spuNumber))
	{
		m_irqPending = true;
	}
	m_irqWatcher->ClearIrqPending(m_spuNumber);

	if(m_volumeAdjust != 1.0f)
	{
		for(int i = 0; i < sampleCount; i++)
		{
			float adjustedSample = static_cast<float>(samplesBase[i]) * m_volumeAdjust;
			adjustedSample = std::clamp<float>(adjustedSample, SHRT_MIN, SHRT_MAX);
			samplesBase[i] = static_cast<int16>(adjustedSample);
		}
	}
}

uint32 CSpuBase::GetAdsrDelta(unsigned int index) const
{
	return m_adsrLogTable[index + 32];
}

//The work area address `delta` bytes from the buffer address plus a reverb register's offset,
//wrapped into [start, end).
uint32 CSpuBase::GetReverbAddress(uint32 address, int32 delta) const
{
	uint32 size = m_reverbWorkAddrEnd - m_reverbWorkAddrStart;
	int64 relative = static_cast<int64>(m_reverbCurrAddr - m_reverbWorkAddrStart) + address + delta;
	relative %= size;
	if(relative < 0) relative += size;
	return m_reverbWorkAddrStart + static_cast<uint32>(relative);
}

float CSpuBase::GetReverbSample(uint32 address, int32 delta) const
{
	return static_cast<float>(*reinterpret_cast<int16*>(m_ram + GetReverbAddress(address, delta)));
}

void CSpuBase::SetReverbSample(uint32 address, float value)
{
	value = std::max<float>(value, SHRT_MIN);
	value = std::min<float>(value, SHRT_MAX);
	*reinterpret_cast<int16*>(m_ram + GetReverbAddress(address, 0)) = static_cast<int16>(value);
}

uint32 CSpuBase::GetReverbOffset(unsigned int registerId) const
{
	return m_reverb[registerId];
}

float CSpuBase::GetReverbCoef(unsigned int registerId) const
{
	int16 value = static_cast<int16>(m_reverb[registerId]);
	return static_cast<float>(value) / static_cast<float>(0x8000);
}

void CSpuBase::UpdateAdsr(CHANNEL& channel)
{
	static const unsigned int logIndex[8] = {0, 4, 6, 8, 9, 10, 11, 12};
	int32 currentAdsrLevel = channel.adsrVolume;
	if(channel.status == ATTACK)
	{
		if(channel.adsrLevel.attackMode == 0)
		{
			//Linear mode
			currentAdsrLevel += GetAdsrDelta((channel.adsrLevel.attackRate ^ 0x7F) - 0x10);
		}
		else
		{
			if(currentAdsrLevel < 0x60000000)
			{
				currentAdsrLevel += GetAdsrDelta((channel.adsrLevel.attackRate ^ 0x7F) - 0x10);
			}
			else
			{
				currentAdsrLevel += GetAdsrDelta((channel.adsrLevel.attackRate ^ 0x7F) - 0x18);
			}
		}
		//Terminasion condition
		if(currentAdsrLevel < 0)
		{
			currentAdsrLevel = MAX_ADSR_VOLUME;
			channel.status = DECAY;
		}
	}
	else if(channel.status == DECAY)
	{
		unsigned int decayType = (static_cast<uint32>(currentAdsrLevel) >> 28) & 0x7;
		currentAdsrLevel -= GetAdsrDelta((4 * (channel.adsrLevel.decayRate ^ 0x1F)) - 0x18 + logIndex[decayType]);
		//Terminasion condition
		if(static_cast<unsigned int>((currentAdsrLevel >> 27) & 0xF) <= channel.adsrLevel.sustainLevel)
		{
			channel.status = SUSTAIN;
		}
	}
	else if(channel.status == SUSTAIN)
	{
		if(channel.adsrRate.sustainDirection == 0)
		{
			//Increment
			if(channel.adsrRate.sustainMode == 0)
			{
				currentAdsrLevel += GetAdsrDelta((channel.adsrRate.sustainRate ^ 0x7F) - 0x10);
			}
			else
			{
				if(currentAdsrLevel < 0x60000000)
				{
					currentAdsrLevel += GetAdsrDelta((channel.adsrRate.sustainRate ^ 0x7F) - 0x10);
				}
				else
				{
					currentAdsrLevel += GetAdsrDelta((channel.adsrRate.sustainRate ^ 0x7F) - 0x18);
				}
			}

			if(currentAdsrLevel < 0)
			{
				currentAdsrLevel = MAX_ADSR_VOLUME;
			}
		}
		else
		{
			//Decrement
			if(channel.adsrRate.sustainMode == 0)
			{
				//Linear
				currentAdsrLevel -= GetAdsrDelta((channel.adsrRate.sustainRate ^ 0x7F) - 0x0F);
			}
			else
			{
				unsigned int sustainType = (static_cast<uint32>(currentAdsrLevel) >> 28) & 0x7;
				currentAdsrLevel -= GetAdsrDelta((channel.adsrRate.sustainRate ^ 0x7F) - 0x1B + logIndex[sustainType]);
			}

			if(currentAdsrLevel < 0)
			{
				currentAdsrLevel = 0;
			}
		}
	}
	else if(channel.status == RELEASE)
	{
		if(channel.adsrRate.releaseMode == 0)
		{
			//Linear
			currentAdsrLevel -= GetAdsrDelta((4 * (channel.adsrRate.releaseRate ^ 0x1F)) - 0x0C);
		}
		else
		{
			unsigned int releaseType = (static_cast<uint32>(currentAdsrLevel) >> 28) & 0x7;
			currentAdsrLevel -= GetAdsrDelta((4 * (channel.adsrRate.releaseRate ^ 0x1F)) - 0x18 + logIndex[releaseType]);
		}

		if(currentAdsrLevel < 0)
		{
			currentAdsrLevel = 0;
			channel.status = STOPPED;
		}
	}
	channel.adsrVolume = static_cast<uint32>(currentAdsrLevel);
}

//The reverb as the PlayStation SPU documentation (psx-spx, "SPU Reverb Formula") gives it, which
//the SPU2 runs per core: at half the output rate, on the mean of each pair of input samples, its
//result held for the pair and scaled by the effect volume (EVOL). Register names are psx-spx's.
void CSpuBase::UpdateReverb(int16 reverbSample[2], int16* samples)
{
	if((m_reverbTicks & 1) == 0)
	{
		m_reverbIn[0] = reverbSample[0];
		m_reverbIn[1] = reverbSample[1];
	}
	else
	{
		const auto R = [&](unsigned int reg, int32 delta = 0) { return GetReverbSample(GetReverbOffset(reg), delta); };
		const auto W = [&](unsigned int reg, float value) { SetReverbSample(GetReverbOffset(reg), value); };
		const float vIIR = GetReverbCoef(IIR_ALPHA);
		const float vWALL = GetReverbCoef(IIR_COEF);
		const float vAPF1 = GetReverbCoef(FB_ALPHA);
		const float vAPF2 = GetReverbCoef(FB_X);
		const float Lin = GetReverbCoef(IN_COEF_L) * (m_reverbIn[0] + reverbSample[0]) * 0.5f;
		const float Rin = GetReverbCoef(IN_COEF_R) * (m_reverbIn[1] + reverbSample[1]) * 0.5f;

		//Same side and different side reflection: mLSAME, mRSAME, mLDIFF, mRDIFF
		W(IIR_DEST_A0, (Lin + R(IIR_SRC_A0) * vWALL - R(IIR_DEST_A0, -2)) * vIIR + R(IIR_DEST_A0, -2));
		W(IIR_DEST_A1, (Rin + R(IIR_SRC_A1) * vWALL - R(IIR_DEST_A1, -2)) * vIIR + R(IIR_DEST_A1, -2));
		W(IIR_DEST_B0, (Lin + R(IIR_SRC_B1) * vWALL - R(IIR_DEST_B0, -2)) * vIIR + R(IIR_DEST_B0, -2));
		W(IIR_DEST_B1, (Rin + R(IIR_SRC_B0) * vWALL - R(IIR_DEST_B1, -2)) * vIIR + R(IIR_DEST_B1, -2));

		//Early echo (comb filter): mLCOMB1-4, mRCOMB1-4
		float out[2];
		for(int c = 0; c < 2; c++)
		{
			out[c] = GetReverbCoef(ACC_COEF_A) * R(ACC_SRC_A0 + c) + GetReverbCoef(ACC_COEF_B) * R(ACC_SRC_B0 + c) +
			         GetReverbCoef(ACC_COEF_C) * R(ACC_SRC_C0 + c) + GetReverbCoef(ACC_COEF_D) * R(ACC_SRC_D0 + c);
			//Late reverb, two all pass filters: mLAPF1 (dAPF1), mLAPF2 (dAPF2)
			const unsigned int apf[2] = {MIX_DEST_A0 + c, MIX_DEST_B0 + c};
			const float coef[2] = {vAPF1, vAPF2};
			const unsigned int delay[2] = {FB_SRC_A, FB_SRC_B};
			for(int i = 0; i < 2; i++)
			{
				const float tap = R(apf[i], -static_cast<int32>(GetReverbOffset(delay[i])));
				out[c] -= coef[i] * tap;
				W(apf[i], out[c]);
				out[c] = out[c] * coef[i] + tap;
			}
		}
		m_reverbOut[0] = out[0] * m_reverbOutVolL / 32768.0f;
		m_reverbOut[1] = out[1] * m_reverbOutVolR / 32768.0f;

		m_reverbCurrAddr += 2;
		if(m_reverbCurrAddr >= m_reverbWorkAddrEnd)
		{
			m_reverbCurrAddr = m_reverbWorkAddrStart;
		}
	}

	for(int c = 0; c < 2; c++)
	{
		int32 resultSample = static_cast<int32>(m_reverbOut[c]) + static_cast<int32>(samples[c]);
		samples[c] = static_cast<int16>(std::clamp<int32>(resultSample, SHRT_MIN, SHRT_MAX));
	}

	m_reverbTicks++;
}

///////////////////////////////////////////////////////
// CSpuSampleCache
///////////////////////////////////////////////////////

const CSpuSampleCache::ITEM* CSpuSampleCache::GetItem(const KEY& key) const
{
	auto range = m_cache.equal_range(key.address);
	for(auto itemIterator = range.first; itemIterator != range.second; itemIterator++)
	{
		const auto& item = itemIterator->second;
		if((item.inS1 == key.s1) && (item.inS2 == key.s2))
		{
			return &item;
		}
	}
	return nullptr;
}

CSpuSampleCache::ITEM& CSpuSampleCache::RegisterItem(const KEY& key)
{
	auto result = m_cache.emplace(std::make_pair(key.address, ITEM()));
	auto& item = result->second;
	item.inS1 = key.s1;
	item.inS2 = key.s2;
	return item;
}

void CSpuSampleCache::Clear()
{
	m_cache.clear();
}

void CSpuSampleCache::ClearRange(uint32 address, uint32 size)
{
	auto lowerBound = m_cache.lower_bound(address);
	auto upperBound = m_cache.upper_bound(address + size);
	m_cache.erase(lowerBound, upperBound);
}

///////////////////////////////////////////////////////
// CSpuIrqWatcher
///////////////////////////////////////////////////////

void CSpuIrqWatcher::Reset()
{
	for(int i = 0; i < MAX_CORES; i++)
	{
		m_irqPending[i] = false;
		m_irqAddr[i] = RESET_IRQ_ADDR;
	}
}

void CSpuIrqWatcher::LoadState(Framework::CZipArchiveReader& archive)
{
	auto registerFile = CRegisterStateFile(*archive.BeginReadFile(STATE_IRQWATCHER_REGS_PATH));
	m_irqAddr[0] = registerFile.GetRegister32(STATE_IRQWATCHER_REGS_IRQADDR0);
	m_irqAddr[1] = registerFile.GetRegister32(STATE_IRQWATCHER_REGS_IRQADDR1);
	m_irqPending[0] = registerFile.GetRegister32(STATE_IRQWATCHER_REGS_IRQPENDING0) != 0;
	m_irqPending[1] = registerFile.GetRegister32(STATE_IRQWATCHER_REGS_IRQPENDING1) != 0;
}

void CSpuIrqWatcher::SaveState(Framework::CZipArchiveWriter& archive)
{
	auto registerFile = std::make_unique<CRegisterStateFile>(STATE_IRQWATCHER_REGS_PATH);
	registerFile->SetRegister32(STATE_IRQWATCHER_REGS_IRQADDR0, m_irqAddr[0]);
	registerFile->SetRegister32(STATE_IRQWATCHER_REGS_IRQADDR1, m_irqAddr[1]);
	registerFile->SetRegister32(STATE_IRQWATCHER_REGS_IRQPENDING0, m_irqPending[0]);
	registerFile->SetRegister32(STATE_IRQWATCHER_REGS_IRQPENDING1, m_irqPending[1]);
	archive.InsertFile(std::move(registerFile));
}

void CSpuIrqWatcher::SetIrqAddress(int core, uint32 address)
{
	m_irqAddr[core] = address;
}

void CSpuIrqWatcher::CheckIrq(uint32 address)
{
	for(int i = 0; i < MAX_CORES; i++)
	{
		if(address == m_irqAddr[i])
		{
			m_irqPending[i] = true;
		}
	}
}

void CSpuIrqWatcher::ClearIrqPending(int core)
{
	m_irqPending[core] = false;
}

bool CSpuIrqWatcher::HasPendingIrq(int core) const
{
	return m_irqPending[core];
}

///////////////////////////////////////////////////////
// CSampleReader
///////////////////////////////////////////////////////

CSpuBase::CSampleReader::CSampleReader()
{
	Reset();
}

void CSpuBase::CSampleReader::Reset()
{
	m_nextSampleAddr = 0;
	m_repeatAddr = 0;
	memset(m_buffer, 0, sizeof(m_buffer));
	m_pitch = 0;
	m_srcSampleIdx = 0;
	m_srcSamplingRate = 0;
	m_dstSamplingRate = 0;
	m_sampleStep = 0;
	m_s1 = 0;
	m_s2 = 0;
	m_done = false;
	m_didChangeRepeat = false;
	m_nextValid = false;
	m_endFlag = false;
}

void CSpuBase::CSampleReader::SetMemory(uint8* ram, uint32 ramSize)
{
	m_ram = ram;
	m_ramSize = ramSize;
	assert((ramSize & (ramSize - 1)) == 0);
}

void CSpuBase::CSampleReader::SetSampleCache(CSpuSampleCache* sampleCache)
{
	m_sampleCache = sampleCache;
}

void CSpuBase::CSampleReader::SetIrqWatcher(CSpuIrqWatcher* irqWatcher)
{
	m_irqWatcher = irqWatcher;
}

void CSpuBase::CSampleReader::SetDestinationSamplingRate(uint32 samplingRate)
{
	m_dstSamplingRate = samplingRate;
	UpdateSampleStep();
}

void CSpuBase::CSampleReader::LoadState(const CRegisterState& channelState)
{
	m_srcSampleIdx = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_SRCSAMPLEIDX);
	m_srcSamplingRate = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_SRCSAMPLINGRATE);
	m_nextSampleAddr = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_NEXTSAMPLEADDR);
	m_repeatAddr = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_REPEATADDR);
	m_pitch = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_PITCH);
	m_s1 = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_S1);
	m_s2 = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_S2);
	m_done = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_DONE) != 0;
	m_nextValid = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_NEXTVALID) != 0;
	m_endFlag = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_ENDFLAG) != 0;
	m_didChangeRepeat = channelState.GetRegister32(STATE_SAMPLEREADER_REGS_DIDCHANGEREPEAT) != 0;
	RegisterStateUtils::ReadArray(channelState, m_buffer, STATE_SAMPLEREADER_REGS_BUFFER_FORMAT);

	UpdateSampleStep();
}

void CSpuBase::CSampleReader::SaveState(CRegisterState& channelState) const
{
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_SRCSAMPLEIDX, m_srcSampleIdx);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_SRCSAMPLINGRATE, m_srcSamplingRate);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_NEXTSAMPLEADDR, m_nextSampleAddr);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_REPEATADDR, m_repeatAddr);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_PITCH, m_pitch);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_S1, m_s1);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_S2, m_s2);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_DONE, m_done);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_NEXTVALID, m_nextValid);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_ENDFLAG, m_endFlag);
	channelState.SetRegister32(STATE_SAMPLEREADER_REGS_DIDCHANGEREPEAT, m_didChangeRepeat);
	RegisterStateUtils::WriteArray(channelState, m_buffer, STATE_SAMPLEREADER_REGS_BUFFER_FORMAT);
}

void CSpuBase::CSampleReader::SetParams(uint32 address, uint32 repeat)
{
	m_srcSampleIdx = 0;
	memset(m_history, 0, sizeof(m_history));
	m_nextSampleAddr = address & (m_ramSize - 1);
	m_repeatAddr = repeat & (m_ramSize - 1);
	m_s1 = 0;
	m_s2 = 0;
	m_nextValid = false;
	m_done = false;
	m_didChangeRepeat = false;
}

void CSpuBase::CSampleReader::SetParamsRead(uint32 address, uint32 repeat)
{
	SetParams(address, repeat);
	AdvanceBuffer();
}

void CSpuBase::CSampleReader::SetParamsNoRead(uint32 address, uint32 repeat)
{
	SetParams(address, repeat);
	memset(m_buffer, 0, sizeof(m_buffer));
}

void CSpuBase::CSampleReader::SetPitch(uint32 baseSamplingRate, uint16 pitch)
{
	m_srcSamplingRate = baseSamplingRate * pitch;
	UpdateSampleStep();
}

// resonance: FREQ_SPU_INTERP picks the voice interpolation: unset or gaussian, the console's
// 4-point Gaussian (its spectrum matches the PCSX2 reference capture within 0.05 dB over 2-16 kHz,
// where Play!'s linear is up to 9.6 dB too bright); linear, Play!'s two-point linear; cubic,
// 4-point Catmull-Rom; sinc, 8-tap Lanczos (a = 4) windowed sinc.
namespace
{
	enum
	{
		INTERP_LINEAR,
		INTERP_GAUSSIAN,
		INTERP_CUBIC,
		INTERP_SINC,
	};

	int ReadInterpolation()
	{
		const char* value = getenv("FREQ_SPU_INTERP");
		if(value == nullptr) return INTERP_GAUSSIAN;
		if(!strcmp(value, "linear")) return INTERP_LINEAR;
		if(!strcmp(value, "cubic")) return INTERP_CUBIC;
		if(!strcmp(value, "sinc")) return INTERP_SINC;
		return INTERP_GAUSSIAN;
	}

	const int g_interpolation = ReadInterpolation();

	// The SPU's Gaussian table, as the nocash psx-spx documentation lists it ("4-Point Gaussian
	// Interpolation"); the PS2's SPU2 interpolates the same way. Each phase's 4 weights sum to
	// 0x8000 less about 0.4 %.
	const int16 g_gauss[512] = {
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
		3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
		39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
		84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
		156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
		266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
		424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
		643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
		935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
		1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
		1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
		2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
		3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
		3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
		4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
		5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
		7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
		8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
		9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
		11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
		12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
		14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
		15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
		17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
		18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
		19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
		21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
		21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
		22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
		22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
	};

	// Taps k = -3..4 around the sample before the read position, for 256 fractions, summing to 1 << 14.
	struct SincTable
	{
		int16 taps[256][8];
		SincTable()
		{
			for(int phase = 0; phase < 256; phase++)
			{
				double weights[8];
				double sum = 0;
				for(int k = 0; k < 8; k++)
				{
					const double x = (k - 3) - phase / 256.0;
					const auto sinc = [](double v) { return v == 0 ? 1.0 : sin(M_PI * v) / (M_PI * v); };
					weights[k] = sinc(x) * sinc(x / 4);
					sum += weights[k];
				}
				for(int k = 0; k < 8; k++) taps[phase][k] = static_cast<int16>(lrint(weights[k] / sum * 16384));
			}
		}
	};
}

int32 CSpuBase::CSampleReader::Interpolate(uint32 index, int32 alpha) const
{
	const auto at = [&](int position) -> int32 { return position < 0 ? m_history[4 + position] : m_buffer[position]; };
	const int i = static_cast<int>(index);
	int32 result;
	if(g_interpolation == INTERP_GAUSSIAN)
	{
		// psx-spx: i is the fraction's top 8 bits; oldest, older, old, new are the samples before,
		// at, and after the read position, and each product is shifted down 15 bits.
		const int g = alpha >> 4;
		result = ((g_gauss[0xff - g] * at(i - 1)) >> 15) + ((g_gauss[0x1ff - g] * at(i)) >> 15) +
		         ((g_gauss[0x100 + g] * at(i + 1)) >> 15) + ((g_gauss[g] * at(i + 2)) >> 15);
	}
	else if(g_interpolation == INTERP_CUBIC)
	{
		const float t = alpha / static_cast<float>(PITCH_BASE);
		const float pm1 = at(i - 1), p0 = at(i), p1 = at(i + 1), p2 = at(i + 2);
		result = static_cast<int32>(lrintf(p0 + 0.5f * t * (p1 - pm1 + t * (2 * pm1 - 5 * p0 + 4 * p1 - p2 + t * (3 * (p0 - p1) + p2 - pm1)))));
	}
	else
	{
		static const SincTable table;
		const int16* taps = table.taps[alpha >> 4];
		int32 sum = 0;
		for(int k = 0; k < 8; k++) sum += at(i - 3 + k) * taps[k];
		result = (sum + (1 << 13)) >> 14;
	}
	return std::clamp<int32>(result, SHRT_MIN, SHRT_MAX);
}

int32 CSpuBase::CSampleReader::GetSample()
{
	uint32 srcSampleIdx = m_srcSampleIdx / PITCH_BASE;
	int32 srcSampleAlpha = m_srcSampleIdx % PITCH_BASE;
	int32 resultSample;
	if(g_interpolation == INTERP_LINEAR)
	{
		int32 currentSample = m_buffer[srcSampleIdx];
		int32 nextSample = m_buffer[srcSampleIdx + 1];
		resultSample = (currentSample * (PITCH_BASE - srcSampleAlpha) / PITCH_BASE) +
		               (nextSample * srcSampleAlpha / PITCH_BASE);
	}
	else
	{
		resultSample = Interpolate(srcSampleIdx, srcSampleAlpha);
	}
	m_srcSampleIdx += m_sampleStep;
	if(srcSampleIdx >= BUFFER_SAMPLES)
	{
		m_srcSampleIdx -= BUFFER_SAMPLES * PITCH_BASE;
		AdvanceBuffer();
	}
	return resultSample;
}

void CSpuBase::CSampleReader::AdvanceBuffer()
{
	if(m_nextValid)
	{
		memcpy(m_history, m_buffer + BUFFER_SAMPLES - 4, sizeof(m_history));
		memmove(m_buffer, m_buffer + BUFFER_SAMPLES, sizeof(int16) * BUFFER_SAMPLES);
		UnpackSamples(m_buffer + BUFFER_SAMPLES);
	}
	else
	{
		UnpackSamples(m_buffer);
		UnpackSamples(m_buffer + BUFFER_SAMPLES);
		m_nextValid = true;
	}
}

void CSpuBase::CSampleReader::UpdateSampleStep()
{
	m_sampleStep = m_srcSamplingRate / m_dstSamplingRate;
}

void CSpuBase::CSampleReader::UnpackSamples(int16* dst)
{
	int32 workBuffer[BUFFER_SAMPLES];

	const uint8* nextSample = m_ram + m_nextSampleAddr;

	m_irqWatcher->CheckIrq(m_nextSampleAddr);

	//Read header
	uint8 shiftFactor = nextSample[0] & 0xF;
	uint8 predictNumber = nextSample[0] >> 4;
	uint8 flags = nextSample[1];
	assert(predictNumber < 5);

	auto cacheKey = CSpuSampleCache::KEY{m_nextSampleAddr, m_s1, m_s2};
	if(predictNumber == 0)
	{
		cacheKey.s1 = 0;
		cacheKey.s2 = 0;
	}

	if(auto cacheItem = m_sampleCache->GetItem(cacheKey))
	{
		memcpy(dst, cacheItem->samples, sizeof(int16) * BUFFER_SAMPLES);
		m_s1 = cacheItem->outS1;
		m_s2 = cacheItem->outS2;
	}
	else
	{
		//Get intermediate values
		{
			unsigned int workBufferPtr = 0;
			for(unsigned int i = 2; i < 16; i++)
			{
				uint8 sampleByte = nextSample[i];
				int16 firstSample = ((sampleByte & 0x0F) << 12);
				int16 secondSample = ((sampleByte & 0xF0) << 8);
				firstSample >>= shiftFactor;
				secondSample >>= shiftFactor;
				workBuffer[workBufferPtr++] = firstSample;
				workBuffer[workBufferPtr++] = secondSample;
			}
		}

		//Generate PCM samples
		{
			// clang-format off
			//Table is 16 entries long to prevent reading indeterminate
			//values if predictNumber is greater or equal to 5.
			//According to some sources, entries at 5 and beyond contain 0 on real hardware
			static const int32 predictorTable[16][2] =
			{
				{0, 0},
				{60, 0},
				{115, -52},
				{98, -55},
				{122, -60},
			};
			// clang-format on

			for(unsigned int i = 0; i < BUFFER_SAMPLES; i++)
			{
				int32 currentValue = workBuffer[i] * 64;
				currentValue += (m_s1 * predictorTable[predictNumber][0]) / 64;
				currentValue += (m_s2 * predictorTable[predictNumber][1]) / 64;
				m_s2 = m_s1;
				m_s1 = currentValue;
				int32 result = (currentValue + 32) / 64;
				result = std::max<int32>(result, SHRT_MIN);
				result = std::min<int32>(result, SHRT_MAX);
				dst[i] = static_cast<int16>(result);
			}

			auto& cacheItem = m_sampleCache->RegisterItem(cacheKey);
			memcpy(&cacheItem.samples, dst, sizeof(int16) * BUFFER_SAMPLES);
			cacheItem.outS1 = m_s1;
			cacheItem.outS2 = m_s2;
		}
	}

	if(flags & 0x04)
	{
		m_repeatAddr = m_nextSampleAddr;
		m_didChangeRepeat = true;
	}

	m_nextSampleAddr += 0x10;
	assert(m_nextSampleAddr < m_ramSize);
	m_nextSampleAddr &= (m_ramSize - 1);

	if(flags & 0x01)
	{
		m_endFlag = true;
		m_nextSampleAddr = m_repeatAddr;

		//If flags is in { 0x01, 0x05, 0x07 }, mute channel (Xenogears requires that)
		if(flags != 0x03)
		{
			m_done = true;
		}
	}
}

uint32 CSpuBase::CSampleReader::GetRepeat() const
{
	return m_repeatAddr;
}

void CSpuBase::CSampleReader::SetRepeat(uint32 repeatAddr)
{
	m_repeatAddr = repeatAddr & (m_ramSize - 1);
}

uint32 CSpuBase::CSampleReader::GetCurrent() const
{
	//Simulate a kind of progress inside the current sample address.
	//Doesn't need to be accurate, but it needs to change. Needed by Romancing Saga.
	uint32 intraSampleIdx = std::min<uint32>((m_srcSampleIdx / PITCH_BASE) / 2, 0x0E);
	return m_nextSampleAddr + intraSampleIdx;
}

bool CSpuBase::CSampleReader::IsDone() const
{
	return m_done;
}

void CSpuBase::CSampleReader::ClearIsDone()
{
	m_done = false;
}

bool CSpuBase::CSampleReader::GetEndFlag() const
{
	return m_endFlag;
}

void CSpuBase::CSampleReader::ClearEndFlag()
{
	m_endFlag = false;
}

bool CSpuBase::CSampleReader::DidChangeRepeat() const
{
	return m_didChangeRepeat;
}

void CSpuBase::CSampleReader::ClearDidChangeRepeat()
{
	m_didChangeRepeat = false;
}

///////////////////////////////////////////////////////
// CBlockSampleReader
///////////////////////////////////////////////////////

void CSpuBase::CBlockSampleReader::Reset()
{
	m_baseSamplingRate = INIT_SAMPLE_RATE;
	m_dstSamplingRate = 0;
	m_srcSampleIdx = SOUND_INPUT_DATA_SAMPLES * TIME_SCALE;
	m_sampleStep = 0;
}

void CSpuBase::CBlockSampleReader::SetBaseSamplingRate(uint32 baseSamplingRate)
{
	m_baseSamplingRate = baseSamplingRate;
	UpdateSampleStep();
}

void CSpuBase::CBlockSampleReader::SetDestinationSamplingRate(uint32 dstSamplingRate)
{
	m_dstSamplingRate = dstSamplingRate;
	UpdateSampleStep();
}

bool CSpuBase::CBlockSampleReader::GetSpdifBypass()
{
	return m_spdifBypass;
}

void CSpuBase::CBlockSampleReader::SetSpdifBypass(bool spdifBypass)
{
	m_spdifBypass = spdifBypass;
}

bool CSpuBase::CBlockSampleReader::CanReadSamples() const
{
	uint32 sampleIdx = (m_srcSampleIdx / TIME_SCALE);
	if(m_spdifBypass)
	{
		// in S/PDIF bypass mode, we consume 2 adjacent samples per cycle
		// as a stereo pair.
		return (sampleIdx < (SOUND_INPUT_DATA_SAMPLES / 2));
	}
	else
	{
		return (sampleIdx < SOUND_INPUT_DATA_SAMPLES);
	}
}

void CSpuBase::CBlockSampleReader::FillBlock(const uint8* block)
{
	memcpy(m_blockBuffer, block, SOUND_INPUT_DATA_SIZE);
	m_srcSampleIdx = 0;
}

void CSpuBase::CBlockSampleReader::GetSamples(int32 samples[2])
{
	assert(m_sampleStep != 0);

	uint32 srcSampleIdx = m_srcSampleIdx / TIME_SCALE;
	assert(srcSampleIdx < SOUND_INPUT_DATA_SAMPLES);

	auto inputSamples = reinterpret_cast<const int16*>(m_blockBuffer);

	if(m_spdifBypass)
	{
		// in S/PDIF bypass mode, input data contains linear interleaved
		// stereo samples, in a single stream. The L sample should always
		// be at an even-numbered index.
		samples[0] = inputSamples[0x000 + (srcSampleIdx * 2)];
		samples[1] = inputSamples[0x000 + (srcSampleIdx * 2) + 1];
	}
	else
	{
		samples[0] = inputSamples[0x000 + srcSampleIdx];
		samples[1] = inputSamples[0x100 + srcSampleIdx];
	}

	m_srcSampleIdx += m_sampleStep;
}

void CSpuBase::CBlockSampleReader::UpdateSampleStep()
{
	m_sampleStep = (m_baseSamplingRate * TIME_SCALE) / m_dstSamplingRate;
}
