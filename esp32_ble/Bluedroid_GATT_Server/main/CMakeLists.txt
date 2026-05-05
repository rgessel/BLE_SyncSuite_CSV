file(GLOB_RECURSE srcs "main.c" "src/*.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "./include"
                       PRIV_REQUIRES driver esp_timer)
