idf_component_register(
    SRCS "src/power_mgmt.c"
    INCLUDE_DIRS "include"
    REQUIRES driver nvs_flash esp_timer esp_adc main
)

# Include the main directory where config.h is located
idf_component_get_property(main_dir main COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE ${main_dir})