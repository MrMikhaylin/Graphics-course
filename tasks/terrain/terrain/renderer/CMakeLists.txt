add_executable(terrain_renderer
  main.cpp
  App.cpp
  Renderer.cpp
  WorldRenderer.cpp
)

target_link_libraries(terrain_renderer
  PRIVATE glfw etna glm::glm wsi gui scene render_utils)

target_add_shaders(terrain_renderer
  shaders/terrain.vert
  shaders/terrain.frag
)