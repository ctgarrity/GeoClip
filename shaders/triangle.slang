struct VertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
};

static const float2 kPositions[3] = {
    float2( 0.0, -0.5),
    float2( 0.5,  0.5),
    float2(-0.5,  0.5),
};

static const float3 kColors[3] = {
    float3(1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(0.0, 0.0, 1.0),
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexIndex : SV_VertexID)
{
    VertexOutput o;
    o.position = float4(kPositions[vertexIndex], 0.0, 1.0);
    o.color    = kColors[vertexIndex];
    return o;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
