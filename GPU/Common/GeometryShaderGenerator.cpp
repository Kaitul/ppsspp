// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <cstdio>
#include <cstdlib>
#include <locale.h>

#include "Common/StringUtils.h"
#include "Common/GPU/OpenGL/GLFeatures.h"
#include "Common/GPU/ShaderWriter.h"
#include "Common/GPU/thin3d.h"
#include "Core/Config.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"
#include "GPU/Common/ShaderId.h"
#include "GPU/Common/ShaderUniforms.h"
#include "GPU/Common/GeometryShaderGenerator.h"

#undef WRITE

#define WRITE(p, ...) p.F(__VA_ARGS__)

// TODO: Could support VK_NV_geometry_shader_passthrough, though the hardware that supports
// it is already pretty fast at geometry shaders..


bool GenerateGeometryShader(const GShaderID &id, char *buffer, const ShaderLanguageDesc &compat, const Draw::Bugs bugs, std::string *errorString) {
	std::vector<const char*> gl_exts;
	if (ShaderLanguageIsOpenGL(compat.shaderLanguage)) {
		if (gl_extensions.EXT_gpu_shader4) {
			gl_exts.push_back("#extension GL_EXT_gpu_shader4 : enable");
		}
	}

	ShaderWriter p(buffer, compat, ShaderStage::Geometry, gl_exts.data(), gl_exts.size());
	p.C("layout(triangles) in;\n");
	p.C("layout(triangle_strip, max_vertices = 6) out;\n");

	if (compat.shaderLanguage == GLSL_VULKAN) {
		WRITE(p, "\n");
		WRITE(p, "layout (std140, set = 0, binding = 3) uniform baseVars {\n%s};\n", ub_baseStr);
	} else if (compat.shaderLanguage == HLSL_D3D11) {
		WRITE(p, "cbuffer base : register(b0) {\n%s};\n", ub_baseStr);
	}

	std::vector<VaryingDef> varyings, outVaryings;

	if (id.Bit(GS_BIT_DO_TEXTURE)) {
		varyings.push_back(VaryingDef{ "vec3", "v_texcoord", Draw::SEM_TEXCOORD0, 0, "highp" });
		outVaryings.push_back(VaryingDef{ "vec3", "v_texcoordOut", Draw::SEM_TEXCOORD0, 0, "highp" });
	}
	varyings.push_back(VaryingDef{ "vec4", "v_color0", Draw::SEM_COLOR0, 1, "lowp" });
	outVaryings.push_back(VaryingDef{ "vec4", "v_color0Out", Draw::SEM_COLOR0, 1, "lowp" });
	if (id.Bit(GS_BIT_LMODE)) {
		varyings.push_back(VaryingDef{ "vec3", "v_color1", Draw::SEM_COLOR1, 2, "lowp" });
		outVaryings.push_back(VaryingDef{ "vec3", "v_color1Out", Draw::SEM_COLOR1, 2, "lowp" });
	}
	varyings.push_back(VaryingDef{ "float", "v_fogdepth", Draw::SEM_TEXCOORD1, 3, "highp" });
	outVaryings.push_back(VaryingDef{ "float", "v_fogdepthOut", Draw::SEM_TEXCOORD1, 3, "highp" });

	p.BeginGSMain(varyings, outVaryings);

	// Apply culling
	p.C("  bool anyInside = false;\n");
	// And apply manual clipping if necessary.
	if (!gstate_c.Supports(GPU_SUPPORTS_CLIP_DISTANCE)) {
		p.C("  float clip0[3];\n");
	}

	p.C("  for (int i = 0; i < 3; i++) {\n");  // TODO: 3 or gl_in.length()? which will be faster?
	p.C("    vec4 outPos = gl_in[i].gl_Position;\n");
	p.C("    vec3 projPos = outPos.xyz / outPos.w;\n");
	p.C("    float projZ = (projPos.z - u_depthRange.z) * u_depthRange.w;\n");
	// Vertex range culling doesn't happen when Z clips, note sign of w is important.
	p.C("    if (u_cullRangeMin.w <= 0.0 || projZ * outPos.w > -outPos.w) {\n");
	const char *outMin = "projPos.x < u_cullRangeMin.x || projPos.y < u_cullRangeMin.y";
	const char *outMax = "projPos.x > u_cullRangeMax.x || projPos.y > u_cullRangeMax.y";
	p.F("      if ((%s) || (%s)) {\n", outMin, outMax);
	p.C("        return;\n");  // Cull!
	p.C("      }\n");
	p.C("    }\n");
	p.C("    if (u_cullRangeMin.w <= 0.0) {\n");
	p.C("      if (projPos.z < u_cullRangeMin.z || projPos.z > u_cullRangeMax.z) {\n");
	// When not clamping depth, cull the triangle of Z is outside the valid range (not based on clip Z.)
	p.C("        return;\n");
	p.C("      }\n");
	p.C("    } else {\n");
	p.C("      if (projPos.z >= u_cullRangeMin.z) { anyInside = true; }\n");
	p.C("      if (projPos.z <= u_cullRangeMax.z) { anyInside = true; }\n");
	p.C("    }\n");

	if (!gstate_c.Supports(GPU_SUPPORTS_CLIP_DISTANCE)) {
		// This is basically the same value as gl_ClipDistance would take, z + w.
		// TODO: Ignore triangles from GE_PRIM_RECTANGLES in transform mode, which should not clip to neg z.
		p.F("    clip0[i] = projZ * outPos.w + outPos.w;\n");
	}

	p.C("  }  // for\n");

	// Cull any triangle fully outside in the same direction when depth clamp enabled.
	// Basically simulate cull distances.
	p.C("  if (u_cullRangeMin.w > 0.0 && !anyInside) {\n");
	p.C("    return;\n");
	p.C("  }\n");

	if (!gstate_c.Supports(GPU_SUPPORTS_CLIP_DISTANCE)) {
		// Clipping against one half-space cuts a triangle (17/27), culls (7/27), or creates two triangles (3/27).
		p.C("  int indices[4];\n");
		p.C("  float factors[4];\n");
		p.C("  int ind = 0;\n");

		// Pass 1 - clip against first half-space.
		p.C("  for (int i = 0; i < 3; i++) {\n");
		// First, use this vertex if it doesn't need clipping.
		p.C("    if (clip0[i] >= 0.0) {\n");
		p.C("      indices[ind] = i;\n");
		p.C("      factors[ind] = 0.0;\n");
		p.C("      ind++;\n");
		p.C("    }\n");

		// Next, we generate an interpolated vertex if signs differ.
		p.C("    int inext = i == 2 ? 0 : i + 1;\n");
		p.C("    if (clip0[i] * clip0[inext] < 0.0) {\n");
		p.C("      float t = clip0[i] < 0.0 ? clip0[i] / (clip0[i] - clip0[inext]) : 1.0 - (clip0[inext] / (clip0[inext] - clip0[i]));\n");
		p.C("      indices[ind] = i;\n");
		p.C("      factors[ind] = t;\n");
		p.C("      ind++;\n");
		p.C("    }\n");

		p.C("  }\n");

		p.C("  if (ind < 3) {\n");
		p.C("    return;\n");
		p.C("  }\n");

		// Alright, time to actually emit the first triangle.
		p.C("  for (int i = 0; i < 3; i++) {\n");
		p.C("    int idx = indices[i];\n");
		p.C("    float factor = factors[i];\n");
		p.C("    int next = idx == 2 ? 0 : idx + 1;\n");
		p.C("    gl_Position = mix(gl_in[idx].gl_Position, gl_in[next].gl_Position, factor);\n");
		for (size_t i = 0; i < varyings.size(); i++) {
			VaryingDef &in = varyings[i];
			VaryingDef &out = outVaryings[i];
			p.F("    %s = mix(%s[idx], %s[next], factor);\n", outVaryings[i].name, varyings[i].name, varyings[i].name);
		}
		p.C("    EmitVertex();\n");
		p.C("  }\n");

		// Did we end up with additional triangles?  We'll do three points each for the rest.
		p.C("  for (int i = 3; i < ind; i++) {\n");
		p.C("    EndPrimitive();\n");

		// Point one, always index zero.
		p.C("    int idx = indices[0];\n");
		p.C("    float factor = factors[0];\n");
		p.C("    int next = idx == 2 ? 0 : idx + 1;\n");
		p.C("    gl_Position = mix(gl_in[idx].gl_Position, gl_in[next].gl_Position, factor);\n");
		for (size_t i = 0; i < varyings.size(); i++) {
			VaryingDef &in = varyings[i];
			VaryingDef &out = outVaryings[i];
			p.F("    %s = mix(%s[idx], %s[next], factor);\n", outVaryings[i].name, varyings[i].name, varyings[i].name);
		}
		p.C("    EmitVertex();\n");

		// After that, one less than i (basically a triangle fan.)
		p.C("    idx = indices[i - 1];\n");
		p.C("    factor = factors[i - 1];\n");
		p.C("    next = idx == 2 ? 0 : idx + 1;\n");
		p.C("    gl_Position = mix(gl_in[idx].gl_Position, gl_in[next].gl_Position, factor);\n");
		for (size_t i = 0; i < varyings.size(); i++) {
			VaryingDef &in = varyings[i];
			VaryingDef &out = outVaryings[i];
			p.F("    %s = mix(%s[idx], %s[next], factor);\n", outVaryings[i].name, varyings[i].name, varyings[i].name);
		}
		p.C("    EmitVertex();\n");

		// And the new vertex itself.
		p.C("    idx = indices[i];\n");
		p.C("    factor = factors[i];\n");
		p.C("    next = idx == 2 ? 0 : idx + 1;\n");
		p.C("    gl_Position = mix(gl_in[idx].gl_Position, gl_in[next].gl_Position, factor);\n");
		for (size_t i = 0; i < varyings.size(); i++) {
			VaryingDef &in = varyings[i];
			VaryingDef &out = outVaryings[i];
			p.F("    %s = mix(%s[idx], %s[next], factor);\n", outVaryings[i].name, varyings[i].name, varyings[i].name);
		}
		p.C("    EmitVertex();\n");

		p.C("  }\n");
	} else {
		const char *clipSuffix0 = compat.shaderLanguage == HLSL_D3D11 ? "" : "[0]";

		p.C("  for (int i = 0; i < 3; i++) {\n");   // TODO: 3 or gl_in.length()? which will be faster?
		p.C("    vec4 outPos = gl_in[i].gl_Position;\n");
		p.C("    vec3 projPos = outPos.xyz / outPos.w;\n");
		p.C("    float projZ = (projPos.z - u_depthRange.z) * u_depthRange.w;\n");
		// TODO: Ignore triangles from GE_PRIM_RECTANGLES in transform mode, which should not clip to neg z.
		p.F("    gl_ClipDistance%s = projZ * outPos.w + outPos.w;\n", clipSuffix0);
		p.C("    gl_Position = outPos;\n");
		if (gstate_c.Supports(GPU_SUPPORTS_CLIP_DISTANCE)) {
		}

		for (size_t i = 0; i < varyings.size(); i++) {
			VaryingDef &in = varyings[i];
			VaryingDef &out = outVaryings[i];
			p.F("    %s = %s[i];\n", outVaryings[i].name, varyings[i].name);
		}
		// Debug - null the red channel
		//p.C("    if (i == 0) v_color0Out.x = 0.0;\n");
		p.C("    EmitVertex();\n");
		p.C("  }\n");
	}

	p.EndGSMain();

	return true;
}
