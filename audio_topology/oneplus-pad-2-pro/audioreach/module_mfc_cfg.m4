# SPDX-License-Identifier: BSD-3-Clause
# Copyright, Linaro Ltd, 2023
# The upstream AudioReach MFC module with a module configuration blob
# (SND_SOC_AR_TPLG_MODULE_CFG_TYPE) the kernel sends after the output media
# format.  The blob is SectionData."NAME_PREFIX.mfc<index>_cfg" and must be
# the last item of the widget's private data.
include(`util/util.m4') dnl
dnl AR_MODULE_MFC_CFG(index, sgidx, container-idx, iid, maxip-ports, max-op-ports, src-port, dst-port, dst-iid)
define(`AR_MODULE_MFC_CFG',
`'
`SectionVendorTuples."NAME_PREFIX.mfc$1_tuples" {'
`        tokens "audioreach_tokens"'
`'
`        tuples."word.u32_data" {'
`                AR_TKN_U32_MODULE_INSTANCE_ID STR($4)'
`                AR_TKN_U32_MODULE_ID STR(MODULE_ID_MFC)'
`                AR_TKN_U32_MODULE_MAX_IP_PORTS STR($5)'
`                AR_TKN_U32_MODULE_MAX_OP_PORTS STR($6)'
`                AR_TKN_U32_MODULE_SRC_OP_PORT_ID STR($7)'
`                AR_TKN_U32_MODULE_DST_IN_PORT_ID STR($8)'
`                AR_TKN_U32_MODULE_SRC_INSTANCE_ID STR($4)'
`                AR_TKN_U32_MODULE_DST_INSTANCE_ID STR($9)'
`        }'
`}'
`'
`SectionData."NAME_PREFIX.mfc$1_data" {'
`        tuples "NAME_PREFIX.mfc$1_tuples"'
`}'
`'
`SectionWidget."NAME_PREFIX.mfc$1" {'
`        index STR($1)'
`        type "src"'
`        no_pm "true"'
`        subseq "10"'
`        data ['
`                "NAME_PREFIX.sub_graph$2_data"'
`                "NAME_PREFIX.container$3_data"'
`                "NAME_PREFIX.mfc$1_data"'
`                "NAME_PREFIX.mfc$1_cfg"'
`        ]'
`}') dnl
