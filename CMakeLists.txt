cmake_minimum_required(VERSION 3.22)
project(PhosChat VERSION 0.2.1 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(cmake/TelegramCredentials.cmake)
phoschat_configure_telegram_credentials("${PROJECT_BINARY_DIR}/generated")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Sql)
option(PHOSCHAT_USE_KEYCHAIN "Enable OS-backed credential storage if QtKeychain is available" ON)
option(PHOSCHAT_ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)
if(PHOSCHAT_ENABLE_CLANG_TIDY)
    find_program(PHOSCHAT_CLANG_TIDY NAMES clang-tidy clang-tidy-19 REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY
        "${PHOSCHAT_CLANG_TIDY};--checks=-*,clang-analyzer-*,bugprone-use-after-move,bugprone-dangling-handle")
endif()
if(PHOSCHAT_USE_KEYCHAIN)
    find_package(Qt6Keychain QUIET)
endif()

function(phoschat_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(phoschat_core STATIC
    src/core/account_manager.cpp
    src/core/chat_provider.h
    src/core/conversation_manager.cpp
    src/core/message_manager.cpp
    src/providers/irc/irc_directory.cpp
    src/providers/irc/irc_message.cpp
    src/providers/irc/irc_provider.cpp
    src/providers/slack/slack_oauth.cpp
    src/providers/slack/slack_provider.cpp
    src/providers/telegram/telegram_provider.cpp
    src/providers/telegram/telegram_transport.cpp
    src/security/credential_store.cpp
    src/security/telegram_app_credentials.cpp
    src/security/telegram_credentials.cpp
    src/storage/database.cpp
)
target_include_directories(phoschat_core PUBLIC src)
target_include_directories(phoschat_core PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(phoschat_core PUBLIC Qt6::Core Qt6::Network Qt6::Sql)
if(PHOSCHAT_USE_KEYCHAIN AND TARGET qt6keychain)
    target_link_libraries(phoschat_core PRIVATE qt6keychain)
    target_compile_definitions(phoschat_core PRIVATE PHOSCHAT_WITH_KEYCHAIN=1)
else()
    message(STATUS "QtKeychain unavailable or disabled. Credentials are session-only.")
endif()
phoschat_warnings(phoschat_core)

add_library(phoschat_ui STATIC
    src/ui/contacts/MainWindow.cpp
    src/ui/conversations/ConversationController.cpp
    src/ui/conversations/MessageModel.cpp
    src/ui/conversations/MessageView.cpp
    src/ui/discovery/IrcDirectoryDialog.cpp
    src/ui/settings/AccountDialog.cpp
    src/ui/styles/Theme.cpp
    src/ui/windows/ChatWindow.cpp
    src/ui/windows/WindowManager.cpp
)
target_link_libraries(phoschat_ui PUBLIC phoschat_core Qt6::Gui Qt6::Widgets)
phoschat_warnings(phoschat_ui)

add_executable(phoschat WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/app/application_controller.cpp
    src/notifications/notification_manager.cpp
)
target_compile_definitions(phoschat PRIVATE PHOSCHAT_VERSION="${PROJECT_VERSION}")
target_link_libraries(phoschat PRIVATE phoschat_ui)
set_target_properties(phoschat PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME PhosChat
    MACOSX_BUNDLE_GUI_IDENTIFIER io.phoschat.desktop
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)
phoschat_warnings(phoschat)
install(TARGETS phoschat RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" BUNDLE DESTINATION .)

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    foreach(suite IN ITEMS irc storage core telegram telegram_credentials slack)
        add_executable(test_${suite} tests/test_${suite}.cpp)
        target_link_libraries(test_${suite} PRIVATE phoschat_core Qt6::Test)
        phoschat_warnings(test_${suite})
        add_test(NAME phoschat.${suite} COMMAND test_${suite})
        set_tests_properties(phoschat.${suite} PROPERTIES TIMEOUT 40)
    endforeach()
    foreach(suite IN ITEMS ui account_overview irc_accounts)
        add_executable(test_${suite} tests/test_${suite}.cpp)
        target_link_libraries(test_${suite} PRIVATE phoschat_ui Qt6::Test)
        phoschat_warnings(test_${suite})
        add_test(NAME phoschat.${suite} COMMAND test_${suite})
        set_tests_properties(phoschat.${suite} PROPERTIES TIMEOUT 40 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()

include(cmake/PhosChatPackaging.cmake)
