add_executable(weapon_gen
    model_gen.cpp
    weapon_gen_main.cpp
)
target_include_directories(weapon_gen PRIVATE ${CMAKE_SOURCE_DIR}/src)
