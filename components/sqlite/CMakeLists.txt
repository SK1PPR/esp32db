# sqlite: the SQLite amalgamation from third_party/sqlite
# (tools/fetch_third_party.sh) plus a small OS layer for ESP-IDF
# (esp_vfs_sqlite.c). Empty unless KV_ENGINE_SQLITE is selected.
# Requirements are declared unconditionally: they're resolved before Kconfig
# values exist.
set(sqlite_root "${CMAKE_CURRENT_LIST_DIR}/../../third_party/sqlite")
set(srcs)
set(incs)

if(CONFIG_KV_ENGINE_SQLITE)
    if(NOT EXISTS "${sqlite_root}/sqlite3.c")
        message(FATAL_ERROR "SQLite sources missing: run tools/fetch_third_party.sh")
    endif()
    set(srcs "${sqlite_root}/sqlite3.c" "esp_vfs_sqlite.c")
    set(incs "${sqlite_root}" ".")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${incs}
                       PRIV_REQUIRES esp_hw_support freertos)

if(srcs)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        SQLITE_OS_OTHER=1              # OS layer is esp_vfs_sqlite.c
        SQLITE_THREADSAFE=0            # kv:: serialises every call
        SQLITE_DEFAULT_LOCKING_MODE=1  # exclusive: lets WAL work without shared memory
        SQLITE_TEMP_STORE=3            # temp tables/indices always in RAM
        SQLITE_DEFAULT_PAGE_SIZE=4096  # = flash / wear-levelling sector
        SQLITE_MAX_MMAP_SIZE=0
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_DQS=0
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_OMIT_SHARED_CACHE
        SQLITE_OMIT_DEPRECATED)
    # Third-party code: don't fail the build on its warnings.
    set_source_files_properties("${sqlite_root}/sqlite3.c" PROPERTIES COMPILE_OPTIONS "-w")
endif()
