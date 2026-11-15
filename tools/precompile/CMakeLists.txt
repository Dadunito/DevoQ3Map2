# Precompile Shaders: a small window that prebakes DevoGI lighting for many maps
# at once, by running mapcompiler.exe (see precompile.cpp)
if(WIN32)
    add_executable(PrecompileShaders WIN32 precompile.cpp)
    target_link_libraries(PrecompileShaders comctl32)
    set_target_properties(PrecompileShaders PROPERTIES OUTPUT_NAME "Precompile Shaders")
endif()
