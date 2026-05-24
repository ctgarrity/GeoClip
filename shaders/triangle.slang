struct ModelBuf { float4x4 model; };

[[vk::push_constant]]
cbuffer PushConstants
{
    float4x4 proj_view;   // offset  0, 64 bytes
    float3   cam_pos;     // offset 64, 12 bytes
    float    _pad;        // offset 76,  4 bytes
    uint64_t model_bda;   // offset 80,  8 bytes
};

static const float3 kLightDir   = normalize(float3(1.0, 2.0, 1.5));
static const float3 kLightColor = float3(1.0, 0.98, 0.92);
static const float  kAmbient    = 0.05;
static const float  kShininess  = 64.0;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 uv       : TEXCOORD0;
};

struct VertexOutput
{
    float4 position                      : SV_Position;
    [[vk::location(0)]] float3 worldPos;
    [[vk::location(1)]] float3 worldNormal;
    [[vk::location(2)]] float2 uv;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput o;
    float4x4 model = ((ModelBuf*)model_bda)->model;
    float4   wp    = mul(model, float4(input.position, 1.0));
    o.worldPos     = wp.xyz;
    o.position     = mul(proj_view, wp);
    o.worldNormal  = normalize(mul((float3x3)model, input.normal));
    o.uv           = input.uv;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 N = normalize(input.worldNormal);
    float3 L = kLightDir;
    float3 V = normalize(cam_pos - input.worldPos);
    float3 H = normalize(L + V);

    float diff   = max(dot(N, L), 0.0);
    float spec   = pow(max(dot(N, H), 0.0), kShininess);
    float3 color = kLightColor * (kAmbient + diff + 0.4 * spec);
    return float4(color, 1.0);
}
