# SPDX-License-Identifier: BSD-3-Clause
# Copyright, Linaro Ltd, 2023
# Graphs derived from the upstream SM8550-HDK AudioReach topology.
# Board microphone selection is provided by the codec routes and UCM.
include(`audioreach/audioreach.m4')
include(`audioreach/stream-subgraph.m4')
include(`audioreach/device-subgraph.m4')
include(`util/route.m4')
include(`util/mixer.m4')
include(`audioreach/tokens.m4')
include(`audioreach/module_tdm.m4')
include(`audioreach/module_mfc_cfg.m4')

# The hardware speaker interface carries eight discrete TDM slots; the front
# end is stereo, as on the stock firmware, and the device MFC spreads it over
# the slots (see device40.mfc1_cfg below).  The front end is S16_LE, a format
# the q6apm-dai platform driver offers (S16_LE and S24_LE only), and at most
# four channels, all its PCM media format commands carry; outside either the
# stream fails with EINVAL before the DSP is reached.
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-vol-playback.m4, FRONTEND_DAI_MULTIMEDIA1,
	`S16_LE', 48000, 48000, 2, 2,
	0x00004001, 0x00004001, 0x00006001, `110000')

# DisplayPort uses its own PCM; the microphone and speaker PCM numbers stay
# unchanged. The stock AudioReach endpoint uses 24-bit samples.
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-vol-playback.m4, FRONTEND_DAI_MULTIMEDIA2,
	`S24_LE', 48000, 48000, 2, 2,
	0x00004002, 0x00004002, 0x00006030, `110000')

STREAM_SG_PCM_ADD(audioreach/subgraph-stream-capture.m4, FRONTEND_DAI_MULTIMEDIA3,
	`S16_LE', 48000, 48000, 2, 2,
	0x00004003, 0x00004003, 0x00006020, `110000')

DEVICE_SG_ADD(audioreach/subgraph-device-codec-dma-capture.m4, `TX_CODEC_DMA_TX_3', TX_CODEC_DMA_TX_3,
	`S16_LE', 48000, 48000, 2, 2,
	LPAIF_INTF_TYPE_RXTX, CODEC_INTF_IDX_TX3, 0, DATA_FORMAT_FIXED_POINT,
	0x00004009, 0x00004009, 0x00006090)

STREAM_DEVICE_CAPTURE_MIXER(FRONTEND_DAI_MULTIMEDIA3, ``TX_CODEC_DMA_TX_3'')
STREAM_DEVICE_CAPTURE_ROUTE(FRONTEND_DAI_MULTIMEDIA3, ``MultiMedia3 Mixer'', ``TX_CODEC_DMA_TX_3, device120.logger1'')

DEVICE_SG_ADD(audioreach/subgraph-device-display-port-playback.m4, `DISPLAY_PORT_RX_0', DISPLAY_PORT_RX_0,
	`S24_LE', 48000, 48000, 2, 2,
	0, 0, 0, DATA_FORMAT_FIXED_POINT,
	0x00004011, 0x00004011, 0x00006120, `DISPLAY_PORT_RX_0')

STREAM_DEVICE_PLAYBACK_MIXER(DISPLAY_PORT_RX_0, ``DISPLAY_PORT_RX_0'', ``MultiMedia2'')
STREAM_DEVICE_PLAYBACK_ROUTE(DISPLAY_PORT_RX_0, ``DISPLAY_PORT_RX_0 Audio Mixer'', ``MultiMedia2, stream1.logger1'')

# Secondary legacy TDM: LOG -> MFC -> hardware endpoint.
define(`NAME_PREFIX', `device40') dnl
AR_SUBGRAPH(1, SECONDARY_TDM_RX_0, 0x00004010,
	APM_SG_PERF_MODE_LOW_LATENCY, APM_SUB_GRAPH_DIRECTION_TX, APM_SUB_GRAPH_SID_AUDIO_PLAYBACK)
AR_CONTAINER(1, 0x00004010, APM_CONTAINER_TYPE_ID_GC, 8192,
	APM_CONT_GRAPH_POS_STREAM, APM_PROC_DOMAIN_ID_ADSP)
AR_MODULE_LOG(1, 1, 1, 0x00006100, 1, 1, 1, 2, 0x000019ab, 1, 0, 0x00006101)
AR_MODULE_MFC_CFG(1, 1, 1, 0x00006101, 1, 1, 1, 2, 0x00006102)
AR_MODULE_TDM_RX(1, 1, 1, 0x00006102, LPAIF_INTF_TYPE_LPAIF, I2S_INTF_TYPE_SECONDARY,
	DATA_FORMAT_FIXED_POINT, 0xff, 8, 32, 2, 1, 1, 0, 0, 0, `Secondary TDM0')

# Stereo to the eight slots, left on the even slots and right on the odd
# ones: the stock speaker graph's MFC calibration (ACDB module 0x4026,
# PARAM_ID_CHMIXER_COEFF 0x0800101f, one 8x2 table in Q14).  The MFC applies
# a table only when its channel maps equal the stream's, otherwise it falls
# back to its built-in ITU-R BS.775 upmix, which leaves slots 2-7 silent.
# Input: FL FR (the stereo front end).  Output: the kernel's default
# eight-channel map, which the MFC output media format carries:
# FL FR LFE FC LS RS LB RB = 1 2 6 3 4 5 8 9.
# Words are little endian; two u16 fields share a word, low half first.
SectionData."device40.mfc1_cfg" {
	words "0x00000060, 0x01001006, 0x00000000, 0x00000000,
		0x00006101, 0x0800101f, 0x0000003c, 0x00000000,
		0x00000001, 0x00020008,
		0x00020001, 0x00030006, 0x00050004, 0x00090008,
		0x00020001,
		0x00004000, 0x40000000, 0x00004000, 0x40000000,
		0x00004000, 0x40000000, 0x00004000, 0x40000000,
		0x00000000"
}
# Header: blob size 96 including it, SND_SOC_AR_TPLG_MODULE_CFG_TYPE, two
# reserved words.  Parameter: instance 0x6101, id, size 60, error code.
# Payload: one table; 8 outputs, 2 inputs; output map; input map; one Q14
# row per output slot, (left, right): 0x4000 = unity; a pad word keeps the
# APM payload a multiple of eight bytes.

SectionGraph."Secondary TDM0 Hardware Graph" {
	index "1"
	lines [
		"device40.logger1, , Secondary TDM0 Audio Mixer"
		"device40.mfc1, , device40.logger1"
		"device40.tdm_rx1, , device40.mfc1"
		"Secondary TDM0 Playback, , device40.tdm_rx1"
	]
}
STREAM_DEVICE_PLAYBACK_MIXER(SECONDARY_TDM_RX_0, ``Secondary TDM0'', ``MultiMedia1'')
STREAM_DEVICE_PLAYBACK_ROUTE(SECONDARY_TDM_RX_0, ``Secondary TDM0 Audio Mixer'', ``MultiMedia1, stream0.logger1'')
