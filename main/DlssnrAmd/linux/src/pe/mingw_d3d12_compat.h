#pragma once
// mingw's d3d12.h omits the root-signature serialiser's function-pointer type,
// which ImGui's D3D12 backend resolves dynamically. Declaring it here keeps the
// vendored backend byte-identical to upstream - patching a vendored file is how
// it silently diverges the next time it is updated.
#include <d3d12.h>
#ifdef __MINGW32__
typedef HRESULT(WINAPI* PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)(const D3D12_ROOT_SIGNATURE_DESC*,
                                                            D3D_ROOT_SIGNATURE_VERSION,
                                                            ID3DBlob**, ID3DBlob**);
#endif

// The vendored Win32 backend came from a tree that supplies its own module
// handle under this name, and uses it only on the multi-viewport path we do not
// enable. Declared rather than patched, so the vendored file stays untouched.
extern HMODULE dllModule;

// XeSS's D3D12 header names this interface; mingw's d3d12.h does not declare it.
// Forward declaration is enough: nothing here ever dereferences one.
struct ID3D12PipelineLibrary;
