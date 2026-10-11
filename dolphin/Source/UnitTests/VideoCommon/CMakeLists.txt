add_dolphin_test(VertexLoaderTest VertexLoaderTest.cpp)
# The online chat window driven through ImGui with the mouse (VideoCommon/OnlineChatUI).
add_dolphin_test(OnlineChatUITest OnlineChatUITest.cpp)
target_link_libraries(OnlineChatUITest PRIVATE videocommon imgui)
