# Panel WWW: zrodlem jest webui/index.html. Przy kazdej kompilacji pakujemy go
# gzipem (poziom 9) do build/ i wbudowujemy w firmware - nie trzeba pamietac
# o recznym odswiezaniu pliku .gz po edycji panelu (w SIHOS17 to byl osobny krok).
#
# CMAKE_CURRENT_LIST_DIR zamiast CMAKE_CURRENT_SOURCE_DIR: ESP-IDF wczytuje ten
# plik takze przez include() ze swojego skryptu (faza zbierania zaleznosci),
# gdzie SOURCE_DIR wskazuje na skrypt ESP-IDF, a nie na ten folder.
set(WEB_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../webui/index.html")
set(WEB_GZ  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")

idf_component_register(
    SRCS "webserver.c" "api_handlers.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_timer esp_hw_support esp_app_format driver
             json log app_update nvs_config wifi_manager ld2450 bme688 ota_manager
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT EXISTS "${WEB_SRC}")
        message(FATAL_ERROR "Brak panelu WWW: ${WEB_SRC}")
    endif()
    idf_build_get_property(python PYTHON)
    add_custom_command(
        OUTPUT "${WEB_GZ}"
        COMMAND ${python} -c "import gzip,sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(gzip.compress(d, 9, mtime=0))" "${WEB_SRC}" "${WEB_GZ}"
        DEPENDS "${WEB_SRC}"
        COMMENT "Pakowanie panelu WWW (gzip)"
        VERBATIM
    )
    add_custom_target(webui_gz DEPENDS "${WEB_GZ}")
    add_dependencies(${COMPONENT_LIB} webui_gz)
    target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY)
endif()
