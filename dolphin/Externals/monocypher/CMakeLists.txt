# Monocypher 4.0.2 (https://monocypher.org), src/ and src/optional/ of the release tag, unmodified.
# Online chat's crypto (docs/chat-protocol.md): X25519, BLAKE2b, XChaCha20-Poly1305, Ed25519.
add_library(monocypher STATIC
  monocypher.c
  monocypher.h
  monocypher-ed25519.c
  monocypher-ed25519.h
)
target_include_directories(monocypher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
dolphin_disable_warnings(monocypher)
