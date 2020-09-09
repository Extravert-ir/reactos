
list(APPEND SOURCE
    guid.c
    partition.c
    partmgr.c
    utils.c)

add_library(partmgr MODULE ${SOURCE} partmgr.rc)
set_module_type(partmgr kernelmodedriver)
add_importlibs(partmgr ntoskrnl hal)
add_registry_inf(partmgr_reg.inf)
add_cd_file(TARGET partmgr DESTINATION reactos/system32/drivers NO_CAB FOR all)
