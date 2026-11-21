find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

qt_add_executable(blossom-qt
    emuthread.cpp
    emuthread.hpp
    emuwindow.cpp
    emuwindow.hpp
    main.cpp
    settings.cpp
    settings.hpp
    settingswindow.cpp
    settingswindow.hpp
)

target_link_libraries(blossom-qt PRIVATE
    blossom_core
    blossom_warnings
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

set_target_properties(blossom-qt PROPERTIES
    OUTPUT_NAME blossom
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

install(TARGETS blossom-qt
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
