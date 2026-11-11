add_subdirectory(enet)

dolphin_disable_warnings(enet)
add_library(enet::enet ALIAS enet)
target_include_directories(enet PUBLIC enet/include)

# IPv6 (Brawl Online, ipv6/enet_ipv6.h): upstream's platform file keeps its code, its socket and
# address functions renamed to enet_v4_*; ipv6/enet_ipv6.c defines the real names, which call
# upstream's for IPv4 sockets and do IPv6 for its own.
set(ENET_V4_RENAMES
  enet_socket_create=enet_v4_socket_create
  enet_socket_bind=enet_v4_socket_bind
  enet_socket_get_address=enet_v4_socket_get_address
  enet_socket_connect=enet_v4_socket_connect
  enet_socket_send=enet_v4_socket_send
  enet_socket_receive=enet_v4_socket_receive
  enet_socket_destroy=enet_v4_socket_destroy
  enet_address_set_host_ip=enet_v4_address_set_host_ip
  enet_address_set_host=enet_v4_address_set_host
  enet_address_get_host_ip=enet_v4_address_get_host_ip
  enet_address_get_host=enet_v4_address_get_host
)
set_source_files_properties(enet/win32.c enet/unix.c TARGET_DIRECTORY enet
  PROPERTIES COMPILE_DEFINITIONS "${ENET_V4_RENAMES}")
target_sources(enet PRIVATE ipv6/enet_ipv6.c)
target_include_directories(enet PUBLIC ipv6)
target_compile_definitions(enet PUBLIC ENET_BRAWL_IPV6=1)
