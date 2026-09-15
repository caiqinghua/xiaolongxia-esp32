#pragma once
struct srmodel_list_t {};
srmodel_list_t* esp_srmodel_init(const char*);
char* esp_srmodel_filter(srmodel_list_t*, const char*, const char*);
char* esp_srmodel_get_wake_words(srmodel_list_t*, char*);
