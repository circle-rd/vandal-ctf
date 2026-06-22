idf_component_register(
    SRCS "vandal_election.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event vandal_common
)
