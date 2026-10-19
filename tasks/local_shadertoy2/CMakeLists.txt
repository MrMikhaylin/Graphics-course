add_executable(local_shadertoy2
  main.cpp
  App.cpp
)

target_link_libraries(local_shadertoy2
  PRIVATE glfw etna glm::glm wsi gui)

target_include_directories(local_shadertoy2
  PRIVATE ${CMAKE_SOURCE_DIR}/thirdparty/stb)

target_add_shaders(local_shadertoy2
  shaders/fullscreen.vert
  shaders/albedo.frag
  shaders/material.frag
  shaders/toy.frag
)