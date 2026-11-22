add_executable(synclip-android-launcher launcher.cpp)
add_library(synclip_android SHARED clipboard.cpp)
target_link_libraries(synclip_android PRIVATE synclip_transport)

foreach(target synclip-android-launcher synclip_android)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()
