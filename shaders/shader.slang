struct VSInput {
    float3 inPosition : POSITION;
    float3 inColor : COLOR;
    float2 inTexCoord : TEXCOORD0;
};

struct UniformBuffer {
    float4x4 model;
    float4x4 view;
    float4x4 proj;
};
[[vk::binding(0, 0)]]
ConstantBuffer<UniformBuffer> ubo;

[[vk::binding(1, 0)]]
Sampler2D textureSampler;

struct VSOutput
{
    float4 pos : SV_Position;
    float3 color : COLOR;
    float2 fragTexCoord : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;
    output.pos = mul(ubo.proj, mul(ubo.view, mul(ubo.model, float4(input.inPosition, 1.0))));
    output.color = input.inColor;
    output.fragTexCoord = input.inTexCoord;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET {
    return textureSampler.Sample(vertIn.fragTexCoord);
}
