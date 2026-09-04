struct ObjectData {
    view_proj: mat4x4<f32>,
    model: mat4x4<f32>,
    albedo: vec4<f32>,
};

@group(0) @binding(0) var<uniform> object: ObjectData;

struct VertexInput {
    @location(0) position: vec3<f32>,
    @location(1) normal: vec3<f32>,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) normal: vec3<f32>,
    @location(1) albedo: vec4<f32>,
};

fn normal_matrix(model: mat4x4<f32>) -> mat3x3<f32> {
    let a = model[0].xyz;
    let b = model[1].xyz;
    let c = model[2].xyz;
    let determinant = dot(a, cross(b, c));
    let inverseDeterminant = 1.0 / determinant;
    return mat3x3<f32>(cross(b, c) * inverseDeterminant,
                       cross(c, a) * inverseDeterminant,
                       cross(a, b) * inverseDeterminant);
}

@vertex
fn vs_main(input: VertexInput) -> VertexOutput {
    let world = object.model * vec4<f32>(input.position, 1.0);
    var output: VertexOutput;
    output.position = object.view_proj * world;
    output.normal = normal_matrix(object.model) * input.normal;
    output.albedo = object.albedo;
    return output;
}
