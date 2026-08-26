idf_component_register(SRCS "main.cpp" "metrics.cpp" "ring_buffer.cpp" "audio_sim.cpp" "vad.cpp" "features.cpp" "kws.cpp" "ds_cnn_model_data.cc" "adpcm.cpp" "network.cpp"
                       INCLUDE_DIRS "include"
                       # Naming any REQUIRES turns off main's implicit
                       # depend-on-everything, so the list must be complete.
                       REQUIRES esp-dsp esp-tflite-micro
                                freertos esp_timer esp_hw_support esp_system heap
                                esp_wifi esp_netif esp_event nvs_flash esp-tls)
