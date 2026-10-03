#ifdef _MSC_VER
#include "stdafx.h"
#include "stdafx_d3d12.h"
#include "D3D12VertexProgramDecompiler.h"
#include "D3D12CommonDecompiler.h"
#include "D3D12ShaderConstants.h"
#include "D3D12VertexFetch.h"
#include "D3D12VertexTextures.h"
#include "Emu/System.h"


std::string D3D12VertexProgramDecompiler::getFloatTypeName(size_t elementCount)
{
	return getFloatTypeNameImp(elementCount);
}

std::string D3D12VertexProgramDecompiler::getIntTypeName(size_t elementCount)
{
	return "int4";
}

std::string D3D12VertexProgramDecompiler::getFunction(enum class FUNCTION f)
{
	switch (f)
	{
	case FUNCTION::VERTEX_TEXTURE_FETCH1D:
	case FUNCTION::VERTEX_TEXTURE_FETCH2D:
	case FUNCTION::VERTEX_TEXTURE_FETCH3D:
	case FUNCTION::VERTEX_TEXTURE_FETCHCUBE: return "$t_fetch($0)";
	default: break;
	}
	return getFunctionImp(f);
}

std::string D3D12VertexProgramDecompiler::compareFunction(COMPARE f, std::string_view Op0, std::string_view Op1, bool /*scalar*/)
{
	return compareFunctionImp(f, std::string(Op0), std::string(Op1));
}

void D3D12VertexProgramDecompiler::insertHeader(std::stringstream &OS)
{
	d3d12::insert_shader_constants(OS);
}

void D3D12VertexProgramDecompiler::insertInputs(std::stringstream& OS, const std::vector<ParamType>&)
{
	d3d12::insert_vertex_fetch(OS);
}

void D3D12VertexProgramDecompiler::insertConstants(std::stringstream & OS, const std::vector<ParamType> & constants)
{
	OS << "cbuffer CONSTANT_BUFFER : register(b1)\n";
	OS << "{\n";
	OS << "	float4 vc[468];\n";
	OS << "	uint transform_branch_bits;\n";
	OS << "};\n";
	for (const auto& type : constants)
	{
		if (!type.type.starts_with("sampler")) continue;
		const auto dimension = type.type.substr(7);
		for (const auto& item : type.items)
		{
			const auto unit = std::stoul(item.name.substr(4));
			ensure(unit < 4);
			d3d12::insert_vertex_texture(OS, static_cast<unsigned>(unit), dimension);
		}
	}
}

void D3D12VertexProgramDecompiler::insertOutputs(std::stringstream & OS, const std::vector<ParamType> & outputs)
{
	OS << "struct PixelInput\n";
	OS << "{\n";
	OS << "	float4 dst_reg0 : SV_POSITION;\n";
	OS << "	float4 dst_reg1 : COLOR0;\n";
	OS << "	float4 dst_reg2 : COLOR1;\n";
	OS << "	float4 dst_reg3 : COLOR2;\n";
	OS << "	float4 dst_reg4 : COLOR3;\n";
	OS << "	float4 dst_reg5 : FOG;\n";
	OS << "	float4 dst_reg6 : TEXCOORD9;\n";
	OS << "	float4 dst_reg7 : TEXCOORD0;\n";
	OS << "	float4 dst_reg8 : TEXCOORD1;\n";
	OS << "	float4 dst_reg9 : TEXCOORD2;\n";
	OS << "	float4 dst_reg10 : TEXCOORD3;\n";
	OS << "	float4 dst_reg11 : TEXCOORD4;\n";
	OS << "	float4 dst_reg12 : TEXCOORD5;\n";
	OS << "	float4 dst_reg13 : TEXCOORD6;\n";
	OS << "	float4 dst_reg14 : TEXCOORD7;\n";
	OS << "	float4 dst_reg15 : TEXCOORD8;\n";
	OS << "	float4 dst_userClip0 : SV_ClipDistance0;\n";
	OS << "	float4 dst_userClip1 : SV_ClipDistance1;\n";
	OS << "};\n";
}

static const vertex_reg_info reg_table[] =
{
	{ "gl_Position", false, "dst_reg0", "", false },
	{ "diff_color", true, "dst_reg1", "", false },
	{ "spec_color", true, "dst_reg2", "", false },
	{ "front_diff_color", true, "dst_reg3", "", false },
	{ "front_spec_color", true, "dst_reg4", "", false },
	{ "fogc", true, "dst_reg5", ".xxxx", true },
	{ "gl_ClipDistance[0]", false, "dst_reg5", ".y * userClipFactor[0].x", false, "userClipEnabled[0].x > 0", "0.5", "Out.dst_userClip0.x" },
	{ "gl_ClipDistance[0]", false, "dst_reg5", ".z * userClipFactor[0].y", false, "userClipEnabled[0].y > 0", "0.5", "Out.dst_userClip0.y" },
	{ "gl_ClipDistance[0]", false, "dst_reg5", ".w * userClipFactor[0].z", false, "userClipEnabled[0].z > 0", "0.5", "Out.dst_userClip0.z" },
	//{ "gl_PointSize", false, "dst_reg6", ".x", false },
	{ "gl_ClipDistance[0]", false, "dst_reg6", ".y * userClipFactor[0].w", false, "userClipEnabled[0].w > 0", "0.5", "Out.dst_userClip0.w" },
	{ "gl_ClipDistance[0]", false, "dst_reg6", ".z * userClipFactor[1].x", false, "userClipEnabled[1].x > 0", "0.5", "Out.dst_userClip1.x" },
	{ "gl_ClipDistance[0]", false, "dst_reg6", ".w * userClipFactor[1].y", false, "userClipEnabled[1].y > 0", "0.5", "Out.dst_userClip1.y" },
	{ "tc9", false, "dst_reg6", "", false },
	{ "tc0", true, "dst_reg7", "", false },
	{ "tc1", true, "dst_reg8", "", false },
	{ "tc2", true, "dst_reg9", "", false },
	{ "tc3", true, "dst_reg10", "", false },
	{ "tc4", true, "dst_reg11", "", false },
	{ "tc5", true, "dst_reg12", "", false },
	{ "tc6", true, "dst_reg13", "", false },
	{ "tc7", true, "dst_reg14", "", false },
	{ "tc8", true, "dst_reg15", "", false },
};


void D3D12VertexProgramDecompiler::insertMainStart(std::stringstream & OS)
{
	insert_d3d12_legacy_function(OS, false);

	OS << "PixelInput main(uint vertex_id : SV_VertexID)\n";
	OS << "{\n";

	// Declare temp registers
	for (const ParamType PT : m_parr.params[PF_PARAM_NONE])
	{
		for (const ParamItem &PI : PT.items)
		{
			OS << "	" << PT.type << " " << PI.name;
			if (!PI.value.empty())
				OS << " = " << PI.value;
			else
				OS << " = " << "float4(0., 0., 0., 0.);";
			OS << ";\n";
		}
	}

	// Declare outputs
	for (const ParamType PT : m_parr.params[PF_PARAM_OUT])
	{
		for (const ParamItem &PI : PT.items)
		{
			OS << "	" << PT.type << " " << PI.name;
			if (!PI.value.empty())
				OS << " = " << PI.value;
			else
				OS << " = " << "float4(0., 0., 0., 0.);";
			OS << ";\n";
		}
	}

	for (const ParamType PT : m_parr.params[PF_PARAM_IN])
	{
		for (const ParamItem &PI : PT.items)
		{
			OS << "\tfloat4 " << PI.name << " = read_location(" << PI.location << "u, vertex_id);\n";
		}
	}
}


void D3D12VertexProgramDecompiler::insertMainEnd(std::stringstream & OS)
{
	OS << "	PixelInput Out = (PixelInput)0;\n";

	bool insert_front_diffuse = (rsx_vertex_program.output_mask & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE) != 0;
	bool insert_front_specular = (rsx_vertex_program.output_mask & CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR) != 0;

	bool insert_back_diffuse = (rsx_vertex_program.output_mask & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE) != 0;
	bool insert_back_specular = (rsx_vertex_program.output_mask & CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR) != 0;

	// Declare inside main function
	for (auto &i : reg_table)
	{
		if (m_parr.HasParam(PF_PARAM_OUT, "float4", i.src_reg))
		{
			if (i.name == "front_diff_color")
				insert_front_diffuse = false;

			if (i.name == "front_spec_color")
				insert_front_specular = false;

			std::string condition = (!i.cond.empty()) ? "(" + i.cond + ") " : "";
			std::string output_name = i.dst_alias.empty() ? "Out." + i.src_reg : i.dst_alias;

			if (condition.empty() || i.default_val.empty())
			{
				if (!condition.empty()) condition = "if " + condition;
				OS << "	" << condition << output_name << " = " << i.src_reg << i.src_reg_mask << ";\n";
			}
			else
			{
				//Condition and fallback values provided
				OS << "	" << output_name << " = " << condition << "? " << i.src_reg << i.src_reg_mask << ": " << i.default_val << ";\n";
			}
		}
	}

	//If 2 sided lighting is active and only back is written, copy the value to the front side (Outrun online arcade)
	if (insert_front_diffuse && insert_back_diffuse)
		if (m_parr.HasParam(PF_PARAM_OUT, "float4", "dst_reg1"))
			OS << "	Out.dst_reg3 = dst_reg1;\n";

	if (insert_front_specular && insert_back_specular)
		if (m_parr.HasParam(PF_PARAM_OUT, "float4", "dst_reg2"))
			OS << "	Out.dst_reg4 = dst_reg2;\n";

	OS << "	Out.dst_reg0 = mul(Out.dst_reg0, scaleOffsetMat);\n";
	OS << "	return Out;\n";
	OS << "}\n";
}

D3D12VertexProgramDecompiler::D3D12VertexProgramDecompiler(const RSXVertexProgram &prog) :
	VertexProgramDecompiler(prog), rsx_vertex_program(prog)
{
}
#endif
