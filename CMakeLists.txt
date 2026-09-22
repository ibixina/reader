cmake_minimum_required(VERSION 3.21)
project(ai_paper_reader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_UI "Build Qt6 UI (requires Qt6 Widgets, Network, Pdf)" ON)
option(BUILD_TESTS "Build core tests" ON)

# --- Core library: Qt-free, STL only (+ sqlite3 for storage) ---
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp
  src/document/*.cpp
  src/pdf/TextExtractor.cpp
  src/pdf/PdfSelection.cpp
  src/pdf/PdfDocument.cpp
  src/analysis/*.cpp
  src/search/*.cpp
  src/ai/ContextManager.cpp
  src/ai/References.cpp
  src/ai/RetrievalEngine.cpp
  src/ai/PromptBuilder.cpp
  src/ai/ChatManager.cpp
  src/ai/LlmProvider.cpp
  src/storage/*.cpp
  src/app/ApplicationState.cpp
)
add_library(reader_core STATIC ${CORE_SOURCES})
target_include_directories(reader_core PUBLIC src)
target_compile_options(reader_core PRIVATE -Wall -Wextra -Wpedantic)

# sqlite3: prefer conda env, fall back to system
find_path(SQLITE3_INCLUDE_DIR sqlite3.h
  PATHS $ENV{CONDA_PREFIX}/include /usr/include NO_DEFAULT_PATH)
find_library(SQLITE3_LIBRARY sqlite3
  PATHS $ENV{CONDA_PREFIX}/lib /usr/lib64 /usr/lib NO_DEFAULT_PATH)
if(SQLITE3_INCLUDE_DIR AND SQLITE3_LIBRARY)
  target_include_directories(reader_core PUBLIC ${SQLITE3_INCLUDE_DIR})
  target_link_libraries(reader_core PUBLIC ${SQLITE3_LIBRARY})
else()
  find_package(SQLite3 QUIET)
  if(TARGET SQLite3::SQLite3)
    target_link_libraries(reader_core PUBLIC SQLite3::SQLite3)
  elseif(TARGET SQLite::SQLite3)
    target_link_libraries(reader_core PUBLIC SQLite::SQLite3)
  else()
    message(FATAL_ERROR "sqlite3 development files are required to build reader_core.")
  endif()
endif()
find_package(Threads REQUIRED)
target_link_libraries(reader_core PUBLIC Threads::Threads)

# --- UI + providers (Qt6) ---
if(BUILD_UI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Network Pdf Sql Concurrent)
  find_package(Qt6 QUIET COMPONENTS WebEngineWidgets)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTOUIC ON)
  set(CMAKE_AUTORCC ON)
  file(GLOB UI_SOURCES CONFIGURE_DEPENDS
    src/app/Application.cpp
    src/ui/*.cpp
    src/pdf/PdfRenderer.cpp
    src/pdf/QtPdfEngine.cpp
    src/pdf/PopplerBridge.cpp
    src/pdf/WordBoxes.cpp
    src/ai/OpenAIProvider.cpp
    src/ai/EmbeddingProviderQt.cpp
    app/main.cpp
  )
  # WebPanel is an optional WebEngine frontend. Keep it out of the base
  # source glob so a Qt build without WebEngine still has a valid native UI.
  list(FILTER UI_SOURCES EXCLUDE REGEX "/WebPanel\\.cpp$")
  find_package(PkgConfig QUIET)
  # Prefer pkg-config, fall back to direct lookup (the .pc Requires chain
  # needs freetype/poppler .pc files absent in minimal sysroots).
  if(PkgConfig_FOUND)
    pkg_check_modules(PopplerQt6 IMPORTED_TARGET poppler-qt6)
  endif()
  if(NOT PopplerQt6_FOUND)
    find_path(PopplerQt6_INCLUDE_DIRS poppler-qt6.h PATH_SUFFIXES poppler/qt6)
    find_library(PopplerQt6_LIBRARIES poppler-qt6)
    if(PopplerQt6_INCLUDE_DIRS AND PopplerQt6_LIBRARIES)
      set(PopplerQt6_FOUND TRUE)
    endif()
  endif()
  if(NOT PopplerQt6_FOUND)
    message(FATAL_ERROR "poppler-qt6 dev files required for the UI raster backend.")
  endif()
  if(TARGET PkgConfig::PopplerQt6)
    set(POPPLER_LINK PkgConfig::PopplerQt6)
  else()
    set(POPPLER_LINK ${PopplerQt6_LIBRARIES})
  endif()
  add_executable(paper-reader ${UI_SOURCES})
  target_sources(paper-reader PRIVATE src/ui/chat_resources.qrc)
  target_include_directories(paper-reader PRIVATE src ${PopplerQt6_INCLUDE_DIRS})
  target_link_libraries(paper-reader PRIVATE reader_core ${POPPLER_LINK}
    Qt6::Widgets Qt6::Network Qt6::Pdf Qt6::Sql Qt6::Concurrent)
  target_compile_options(paper-reader PRIVATE -Wall -Wextra -Wpedantic)
  if(TARGET Qt6::WebEngineWidgets)
    target_sources(paper-reader PRIVATE src/ui/WebPanel.cpp)
    target_link_libraries(paper-reader PRIVATE Qt6::WebEngineWidgets)
    # The bundled QtWebEngine package omits its expat dependency from the
    # imported target on some sysroots, so resolve it explicitly at link time.
    # Discard an incompatible cached value from a previous configuration.
    unset(EXPAT_LIBRARY CACHE)
    find_file(EXPAT_LIBRARY NAMES libexpat.so libexpat.so.1
      PATHS /usr/lib64 $ENV{CONDA_PREFIX}/lib /usr/lib
      NO_DEFAULT_PATH)
    if(NOT EXPAT_LIBRARY)
      message(FATAL_ERROR "Qt WebEngine requires an expat runtime library.")
    endif()
    # Keep expat on the link line until QtWebEngineCore's transitive symbols
    # are processed; this sysroot's imported target omits that dependency.
    if(UNIX AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_link_options(paper-reader PRIVATE "-Wl,--no-as-needed")
    endif()
    target_link_libraries(paper-reader PRIVATE ${EXPAT_LIBRARY})
    target_compile_definitions(paper-reader PRIVATE HAVE_WEBENGINE)
    message(STATUS "Qt WebEngine found: embedded ChatGPT tab enabled.")
  else()
    message(STATUS "Qt WebEngine not found: native Chat tab used.")
  endif()
endif()

# --- Tests ---
if(BUILD_TESTS)
  enable_testing()
  set(ISOLATED_TEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunIsolatedTest.cmake")
  set(ISOLATED_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/test-homes")
  add_executable(test_core tests/test_core.cpp)
  target_include_directories(test_core PRIVATE src)
  target_link_libraries(test_core PRIVATE reader_core)
  target_compile_options(test_core PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME core COMMAND ${CMAKE_COMMAND}
    -DTEST_EXECUTABLE=$<TARGET_FILE:test_core>
    -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
    -P ${ISOLATED_TEST_SCRIPT})
  add_executable(test_chat_lifecycle tests/test_chat_lifecycle.cpp)
  target_include_directories(test_chat_lifecycle PRIVATE src)
  target_link_libraries(test_chat_lifecycle PRIVATE reader_core)
  target_compile_options(test_chat_lifecycle PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME chat_lifecycle COMMAND ${CMAKE_COMMAND}
    -DTEST_EXECUTABLE=$<TARGET_FILE:test_chat_lifecycle>
    -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
    -P ${ISOLATED_TEST_SCRIPT})
  if(Qt6_FOUND AND PopplerQt6_FOUND)
    set(NATIVE_QT_TEST_ENV "QT_QPA_PLATFORM=offscreen"
      "LD_LIBRARY_PATH=/usr/lib64:/home/nao/qtsysroot/usr/lib64:/home/nao/miniforge3/lib")
    add_executable(test_qt tests/test_qt.cpp src/pdf/QtPdfEngine.cpp)
    target_include_directories(test_qt PRIVATE src)
    target_link_libraries(test_qt PRIVATE reader_core
      Qt6::Core Qt6::Gui Qt6::Pdf Qt6::Widgets)
    target_compile_options(test_qt PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME qt COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_qt>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(qt PROPERTIES ENVIRONMENT "${NATIVE_QT_TEST_ENV}")
    add_executable(test_select tests/test_select.cpp src/ui/PdfView.cpp
      src/app/Application.cpp src/ai/OpenAIProvider.cpp src/ai/EmbeddingProviderQt.cpp
      src/pdf/PopplerBridge.cpp
      src/pdf/PdfRenderer.cpp src/pdf/WordBoxes.cpp)
    target_include_directories(test_select PRIVATE src tests ${PopplerQt6_INCLUDE_DIRS})
    target_link_libraries(test_select PRIVATE reader_core ${POPPLER_LINK}
      Qt6::Widgets Qt6::Network Qt6::Pdf Qt6::Concurrent)
    target_compile_options(test_select PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME select COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_select>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(select PROPERTIES ENVIRONMENT "${NATIVE_QT_TEST_ENV}")
    add_executable(test_chat_ui tests/test_chat_ui.cpp src/app/Application.cpp
      src/ui/ChatPanel.cpp src/ui/ChatTranscript.cpp src/ai/OpenAIProvider.cpp
      src/ui/chat_resources.qrc)
    target_include_directories(test_chat_ui PRIVATE src)
    target_link_libraries(test_chat_ui PRIVATE reader_core
      Qt6::Widgets Qt6::Concurrent Qt6::Network)
    target_compile_options(test_chat_ui PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME chat_ui COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_chat_ui>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(chat_ui PROPERTIES ENVIRONMENT "${NATIVE_QT_TEST_ENV}")
    add_executable(test_embedding_qt tests/test_embedding_qt.cpp src/ai/EmbeddingProviderQt.cpp)
    target_include_directories(test_embedding_qt PRIVATE src)
    target_link_libraries(test_embedding_qt PRIVATE reader_core Qt6::Core Qt6::Network)
    target_compile_options(test_embedding_qt PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME embedding_qt COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_embedding_qt>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(embedding_qt PROPERTIES ENVIRONMENT "${NATIVE_QT_TEST_ENV}"
      SKIP_REGULAR_EXPRESSION "embedding adapter test skipped")
  endif()
  if(Qt6_FOUND AND PopplerQt6_FOUND)
    set(READER_UI_TEST_SOURCES ${UI_SOURCES})
    list(FILTER READER_UI_TEST_SOURCES EXCLUDE REGEX "/app/main\\.cpp$")
    add_executable(test_reader_ui tests/test_reader_ui.cpp ${READER_UI_TEST_SOURCES})
    target_include_directories(test_reader_ui PRIVATE src tests ${PopplerQt6_INCLUDE_DIRS})
    target_link_libraries(test_reader_ui PRIVATE reader_core ${POPPLER_LINK}
      Qt6::Widgets Qt6::Network Qt6::Pdf Qt6::Sql Qt6::Concurrent)
    target_compile_options(test_reader_ui PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME reader_ui COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_reader_ui>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(reader_ui PROPERTIES ENVIRONMENT "${NATIVE_QT_TEST_ENV}")
  endif()
  if(Qt6_FOUND AND PopplerQt6_FOUND)
    add_executable(test_render tests/test_render.cpp src/pdf/PopplerBridge.cpp)
    target_include_directories(test_render PRIVATE src tests ${PopplerQt6_INCLUDE_DIRS})
    target_link_libraries(test_render PRIVATE ${POPPLER_LINK} Qt6::Gui)
    target_compile_options(test_render PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME render COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_render>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(render PROPERTIES ENVIRONMENT "${NATIVE_QT_TEST_ENV}")
    add_executable(test_render_stress tests/test_render_stress.cpp src/pdf/PopplerBridge.cpp
      src/pdf/PdfRenderer.cpp)
    target_include_directories(test_render_stress PRIVATE src tests ${PopplerQt6_INCLUDE_DIRS})
    target_link_libraries(test_render_stress PRIVATE ${POPPLER_LINK}
      Qt6::Gui Qt6::Core Qt6::Pdf)
    target_compile_options(test_render_stress PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME render_stress COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_render_stress>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(render_stress PROPERTIES ENVIRONMENT "${NATIVE_QT_TEST_ENV}")
  endif()
  if(TARGET Qt6::WebEngineWidgets)
    add_executable(test_webengine tests/test_webengine.cpp src/ui/WebPanel.cpp)
    target_include_directories(test_webengine PRIVATE src)
    target_link_libraries(test_webengine PRIVATE reader_core
      Qt6::Widgets Qt6::WebEngineWidgets ${EXPAT_LIBRARY})
    if(UNIX AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_link_options(test_webengine PRIVATE "-Wl,--no-as-needed")
    endif()
    target_compile_options(test_webengine PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME webengine COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_webengine>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    # Headless Chromium needs sandbox off here; resource overrides only
    # apply on machines carrying the rootless sysroot (harmless elsewhere).
    set(WEBENGINE_TEST_ENV "QT_QPA_PLATFORM=offscreen" "QTWEBENGINE_DISABLE_SANDBOX=1"
        "QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --disable-gpu")
    if(EXISTS "/home/nao/qtsysroot/usr/lib64/qt6/libexec/QtWebEngineProcess")
      list(APPEND WEBENGINE_TEST_ENV
        "LD_LIBRARY_PATH=/usr/lib64:/home/nao/qtsysroot/usr/lib64:/home/nao/miniforge3/lib"
        "QTWEBENGINEPROCESS_PATH=/home/nao/qtsysroot/usr/lib64/qt6/libexec/QtWebEngineProcess"
        "QTWEBENGINE_RESOURCES_PATH=/home/nao/qtsysroot/usr/share/qt6/resources"
        "QTWEBENGINE_LOCALES_PATH=/home/nao/qtsysroot/usr/share/qt6/translations/qtwebengine_locales"
        "QTWEBENGINE_DICTIONARIES_PATH=/home/nao/qtsysroot/usr/share/qt6/qtwebengine_dictionaries")
    endif()
    set_tests_properties(webengine PROPERTIES ENVIRONMENT "${WEBENGINE_TEST_ENV}")
    add_executable(test_chat_rich tests/test_chat_rich.cpp src/ui/ChatTranscript.cpp
      src/ui/chat_resources.qrc)
    target_include_directories(test_chat_rich PRIVATE src)
    target_link_libraries(test_chat_rich PRIVATE Qt6::Widgets Qt6::WebEngineWidgets
      ${EXPAT_LIBRARY})
    target_compile_definitions(test_chat_rich PRIVATE HAVE_WEBENGINE)
    if(UNIX AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_link_options(test_chat_rich PRIVATE "-Wl,--no-as-needed")
    endif()
    target_compile_options(test_chat_rich PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME chat_rich COMMAND ${CMAKE_COMMAND}
      -DTEST_EXECUTABLE=$<TARGET_FILE:test_chat_rich>
      -DTEST_HOME_ROOT=${ISOLATED_TEST_ROOT}
      -P ${ISOLATED_TEST_SCRIPT})
    set_tests_properties(chat_rich PROPERTIES ENVIRONMENT "${WEBENGINE_TEST_ENV}")
  endif()
endif()
