﻿Shader "Hidden/Klinker/Upsampler"
{
    Properties
    {
        _MainTex("", 2D) = "" {}
    }
    SubShader
    {
        Cull Off ZWrite Off ZTest Always
        Pass
        {
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment Fragment_UYVY
            #pragma multi_compile _ UNITY_COLORSPACE_GAMMA
            #include "Upsampler.cginc"
            ENDCG
        }
        Pass
        {
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment Fragment_UYVY
            #pragma multi_compile _ UNITY_COLORSPACE_GAMMA
            #define UPSAMPLER_INTERLACE_ODD
            #include "Upsampler.cginc"
            ENDCG
        }
        Pass
        {
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment Fragment_UYVY
            #pragma multi_compile _ UNITY_COLORSPACE_GAMMA
            #define UPSAMPLER_INTERLACE_EVEN
            #include "Upsampler.cginc"
            ENDCG
        }
    }
}