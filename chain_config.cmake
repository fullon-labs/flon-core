## config cmake
set( PROJECT_NAME                   fullon      )

## config system account
set( SYSTEM_ACCOUNT_NAME            flon        )
set( SYSTEM_ACCOUNT_PREFIX          flon        )

## config core symbol
set( CORE_SYMBOL_NAME               FLON        )
set( CORE_SYMBOL_PRECISION          8           )

## config program name
set( PROGRAM_ROOT_NAME              flon        )
set( CLIENT_EXECUTABLE_NAME         fucli       )
set( NODE_EXECUTABLE_NAME           funod       )
set( KEY_STORE_EXECUTABLE_NAME      fuwal       )
set( UTIL_EXECUTABLE_NAME           flon-util   )

## config crypto keys
set( PUBLIC_KEY_LEGACY_PREFIX       FU          )

# used by add_compile_definitions()
set(CHAIN_CONFIG_DEFINITIONS
    SYSTEM_ACCOUNT_NAME=${SYSTEM_ACCOUNT_NAME}
    SYSTEM_ACCOUNT_PREFIX=${SYSTEM_ACCOUNT_PREFIX}
    CORE_SYMBOL_NAME=${CORE_SYMBOL_NAME}
    CORE_SYMBOL_PRECISION=${CORE_SYMBOL_PRECISION}
    PROGRAM_ROOT_NAME=${PROGRAM_ROOT_NAME}
    CLIENT_EXECUTABLE_NAME=${CLIENT_EXECUTABLE_NAME}
    NODE_EXECUTABLE_NAME=${NODE_EXECUTABLE_NAME}
    KEY_STORE_EXECUTABLE_NAME=${KEY_STORE_EXECUTABLE_NAME}
    UTIL_EXECUTABLE_NAME=${UTIL_EXECUTABLE_NAME}
    PUBLIC_KEY_LEGACY_PREFIX=${PUBLIC_KEY_LEGACY_PREFIX}
)

