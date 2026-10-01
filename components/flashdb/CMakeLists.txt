# flashdb: FlashDB KVDB + its FAL flash layer, built from third_party/FlashDB
# (tools/fetch_third_party.sh). Empty unless KV_ENGINE_FLASHDB is selected.
# Requirements are declared unconditionally: they're resolved before Kconfig
# values exist.
set(fdb_root "${CMAKE_CURRENT_LIST_DIR}/../../third_party/FlashDB")
set(srcs)
set(incs)

if(CONFIG_KV_ENGINE_FLASHDB)
    if(NOT EXISTS "${fdb_root}/src/fdb_kvdb.c")
        message(FATAL_ERROR "FlashDB sources missing: run tools/fetch_third_party.sh")
    endif()
    set(srcs "${fdb_root}/src/fdb.c"
             "${fdb_root}/src/fdb_kvdb.c"
             "${fdb_root}/src/fdb_utils.c"
             "${fdb_root}/port/fal/src/fal.c"
             "${fdb_root}/port/fal/src/fal_flash.c"
             "${fdb_root}/port/fal/src/fal_partition.c"
             "port/fal_port.c")
    set(incs "port" "${fdb_root}/inc" "${fdb_root}/port/fal/inc")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${incs}
                       PRIV_REQUIRES esp_partition log)

if(srcs)
    # Third-party code: don't fail the build on its warnings.
    target_compile_options(${COMPONENT_LIB} PRIVATE -w)
endif()
