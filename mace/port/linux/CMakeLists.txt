add_library(port_linux STATIC
  env.cc
)

target_link_libraries(port_linux port_linux_base)
