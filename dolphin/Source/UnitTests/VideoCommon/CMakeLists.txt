add_dolphin_test(VertexLoaderTest VertexLoaderTest.cpp)
add_dolphin_test(OutputResamplingTest OutputResamplingTest.cpp)
# The online chat window driven through ImGui with the mouse (VideoCommon/OnlineChatUI).
add_dolphin_test(OnlineChatUITest OnlineChatUITest.cpp)
target_link_libraries(OnlineChatUITest PRIVATE videocommon imgui)
# Smooth motion's transform blending and draw matching (VideoCommon/FrameInterpolation).
add_dolphin_test(FrameInterpolationTest FrameInterpolationTest.cpp)
