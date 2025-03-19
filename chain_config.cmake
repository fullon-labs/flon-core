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
set( CLI_CLIENT_EXECUTABLE_NAME     focli       )
set( NODE_EXECUTABLE_NAME           fonod       )
set( KEY_STORE_EXECUTABLE_NAME      fowal       )
set( UTIL_EXECUTABLE_NAME           flon-util   )

## config crypto keys
set( PUBLIC_KEY_LEGACY_PREFIX       FO          )

# used by add_compile_definitions()
set(CHAIN_CONFIG_DEFINITIONS
    SYSTEM_ACCOUNT_NAME=${SYSTEM_ACCOUNT_NAME}
    SYSTEM_ACCOUNT_PREFIX=${SYSTEM_ACCOUNT_PREFIX}
    CORE_SYMBOL_NAME=${CORE_SYMBOL_NAME}
    CORE_SYMBOL_PRECISION=${CORE_SYMBOL_PRECISION}
    PROGRAM_ROOT_NAME=${PROGRAM_ROOT_NAME}
    CLI_CLIENT_EXECUTABLE_NAME=${CLI_CLIENT_EXECUTABLE_NAME}
    NODE_EXECUTABLE_NAME=${NODE_EXECUTABLE_NAME}
    KEY_STORE_EXECUTABLE_NAME=${KEY_STORE_EXECUTABLE_NAME}
    UTIL_EXECUTABLE_NAME=${UTIL_EXECUTABLE_NAME}
    PUBLIC_KEY_LEGACY_PREFIX=${PUBLIC_KEY_LEGACY_PREFIX}
)

