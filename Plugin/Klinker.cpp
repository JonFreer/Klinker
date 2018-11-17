#include "Common.h"
#include "Receiver.h"
#include "Unity/IUnityRenderingExtensions.h"

namespace klinker
{

}

namespace
{


    // Callback for texture update events
    void TextureUpdateCallback(int eventID, void* data)
    {
        auto event = static_cast<UnityRenderingExtEventType>(eventID);

        if (event == kUnityRenderingExtEventUpdateTextureBeginV2)
        {
            // UpdateTextureBegin: Generate and return texture image data.
            auto params = reinterpret_cast<UnityRenderingExtTextureUpdateParamsV2*>(data);
            auto frame = params->userData;

            uint32_t* img = new uint32_t[params->width * params->height];
            for (auto y = 0u; y < params->height; y++)
                for (auto x = 0u; x < params->width; x++)
                    img[y * params->width + x] = x;

            params->texData = img;
        }
        else if (event == kUnityRenderingExtEventUpdateTextureEndV2)
        {
            // UpdateTextureEnd: Free up the temporary memory.
            auto params = reinterpret_cast<UnityRenderingExtTextureUpdateParamsV2*>(data);
            delete[] reinterpret_cast<uint32_t*>(params->texData);
        }
    }
}

extern "C" UnityRenderingEventAndData UNITY_INTERFACE_EXPORT GetTextureUpdateCallback()
{
    return TextureUpdateCallback;
}