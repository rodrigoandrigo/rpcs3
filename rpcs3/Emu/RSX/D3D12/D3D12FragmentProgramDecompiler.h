#pragma once

#include "Emu/RSX/Program/RSXFragmentProgram.h"
#include <sstream>

#include "../Program/FragmentProgramDecompiler.h"

class D3D12FragmentDecompiler : public FragmentProgramDecompiler
{
protected:
	virtual std::string getFloatTypeName(size_t elementCount) override;
	virtual std::string getHalfTypeName(size_t elementCount) override;
	virtual std::string getFunction(enum FUNCTION) override;
	std::string compareFunction(COMPARE, std::string_view, std::string_view) override;

	virtual void insertHeader(std::stringstream &OS) override;
	virtual void insertInputs(std::stringstream &OS) override;
	virtual void insertOutputs(std::stringstream &OS) override;
	virtual void insertConstants(std::stringstream &OS) override;
	virtual void insertGlobalFunctions(std::stringstream &OS) override;
	virtual void insertMainStart(std::stringstream &OS) override;
	virtual void insertMainEnd(std::stringstream &OS) override;
public:
	D3D12FragmentDecompiler(const RSXFragmentProgram &prog, u32& size);
};
