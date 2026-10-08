# Base de implementação Vulkan do Dozen — Mesa 26.2.2

A base registra **125 extensões pendentes** sem anunciá-las. Ela converte os requisitos do `vk.xml` em contratos de trabalho verificáveis; não representa suporte runtime.

## Camadas reutilizáveis

- **registry_and_generated_api**: Define nomes Vulkan, estruturas, aliases e comandos; os geradores Mesa produzem metadados de entrypoints.
  - Limite: Declarações geradas e processamento pNext comum não implementam a semântica D3D12.
- **backend_capability_probe**: Consultar uma capacidade D3D12 real por adaptador e normalizá-la para as consultas Vulkan.
- **vulkan_feature_property_and_dependency_reporting**: Reportar apenas features/properties implementadas e aplicar dependências e regras de versão da API.
- **pnext_and_object_translation**: Consumir cada estrutura de entrada e preservar a validação e a semântica de objetos Vulkan.
- **command_dispatch_and_execution**: Implementar comandos ou comprovar o caminho comum/alias, incluindo falhas e sincronização.
- **shader_translation_and_d3d12_mapping**: Implementar semânticas de shader e recursos que o runtime Vulkan comum não fornece.
- **verification**: Verificar descoberta, habilitação, estruturas, comportamento, dependências e conformidade em runtime real.

## Cobertura do scaffold

- Candidatas revisadas: 177
- Implementadas na matriz: 52
- Pendentes com checklist por extensão: 125
- Pendentes que aparecem anunciadas na fonte: 0
- Extensões não encontradas no `vk.xml`: 0

## Bloqueios registrados

- `feature_and_zero_one_semantics_unimplemented`: 1
- `feature_not_reported`: 59
- `missing_backend_commands`: 47
- `unhandled_type_or_semantics`: 17
- `unsupported_stub_and_feature_false`: 1

## Regra para promover uma extensão

Uma extensão só pode sair da pendência depois que probe de capacidade, feature/property, dependências, estruturas `pNext`, comandos, tradução D3D12/compilador e testes aplicáveis estiverem cobertos. Símbolos gerados ou handlers comuns, isoladamente, não provam que a semântica esteja implementada.

A lista completa e os requisitos de cada extensão estão em `extension_support_scaffold.json`. O teste `test_extension_support_scaffold.py` falha se uma extensão pendente for anunciada sem atualizar a matriz e comprovar sua implementação.

Fora da matriz fixa de 171 candidatas, `VK_KHR_shader_terminate_invocation` foi habilitada na rodada 31: o caminho SPIR-V/NIR do Mesa gera `nir_terminate`, e a lowering DXIL existente o converte em demote seguido de retorno imediato. Essa implementação adicional não altera a contagem da matriz.

A rodada 32 implementa `VK_KHR_zero_initialize_workgroup_memory`, promovida ao Vulkan 1.3: o parser marca shaders SPIR-V com inicializadores nulos de Workgroup; Dozen roteia esses shaders pela lowering de memória compartilhada, distribui stores zero entre as invocações e insere uma barreira de grupo antes do corpo do shader. O backend DXIL traduz shared stores, `LocalInvocationIndex` e a barreira para TGSM. A matriz fixa passa a 24 implementadas e 147 pendentes; sem runtime Windows/D3D12 nem Vulkan CTS, não se reivindica conformidade CTS.

A rodada 33 implementa `VK_KHR_shader_float_controls2` usando o parser SPIR-V/NIR existente, que mapeia `FPFastMathDefault` e `FPFastMathMode` para controles NIR consumidos pelo pipeline SPIR-V→DXIL. A API Vulkan 1.2 Dozen já satisfaz as dependências da extensão e o suporte KHR float controls; a versão da API não foi promovida a 1.4. A matriz fixa passa a 25 implementadas e 146 pendentes, ainda com zero pendentes anunciadas. Os testes estáticos e build/link Linux passaram; sem runtime Windows/D3D12 nem Vulkan CTS, não se reivindica conformidade CTS.


## Rodada 34 — `VK_KHR_shader_relaxed_extended_instruction`

Dozen anuncia `VK_KHR_shader_relaxed_extended_instruction` e reporta `shaderRelaxedExtendedInstruction`. O parser SPIR-V comum descarta `OpExtInstWithForwardRefsKHR` sem tentar resolver seus IDs futuros, também quando a coleta opcional de debug-info está habilitada; o caminho SPIR-V→NIR→DXIL reutiliza esse comportamento. O escopo da extensão é não semântico, então não é necessária uma capacidade específica de hardware D3D12. A matriz revisada passa a **26 implementadas e 145 pendentes**, com zero pendentes anunciadas. Foram aprovados o teste C++ `spirv_tests`, seis testes focais, a suíte estática completa e o build/link Linux; runtime Windows/D3D12 e Vulkan CTS não foram executados.

## Rodada 35 — `VK_EXT_image_robustness`

Dozen anuncia `VK_EXT_image_robustness` e reporta `robustImageAccess=true`. Quando a feature está habilitada, o caminho SPIR-V→NIR→DXIL aplica `nir_lower_robust_access` somente a loads, stores e atomics de imagem: consulta as dimensões da image view, suprime escritas/atomics fora dos limites e devolve o valor format-aware permitido para leituras inválidas. O estado da feature também entra nas chaves de cache NIR de graphics e compute. A semântica segue a [referência oficial da extensão](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_image_robustness.html) e a [Vulkan Guide de robustness](https://docs.vulkan.org/guide/latest/robustness.html).

A matriz fixa passa a **27 implementadas e 144 pendentes**, com zero pendentes anunciadas. Passaram sete testes focais, 210 testes estáticos, `compileall`, a checagem do scaffold e o build/link Linux do ICD. Não houve runtime Windows/D3D12 nem Vulkan CTS; não se reivindica conformidade CTS.

## Rodada 36 — `VK_INTEL_shader_integer_functions2`

Dozen anuncia `VK_INTEL_shader_integer_functions2` e reporta `shaderIntegerFunctions2=true`. A capability `IntegerFunctions2INTEL` fica habilitada no frontend SPIR-V→NIR do DXIL. Os 14 opcodes da extensão são mapeados para NIR; saturação, médias e diferença absoluta usam reduções algébricas existentes, contagem de zeros é reduzida para operações de bits, e `IMul32x16`/`UMul32x16` agora são reduzidos explicitamente para extração dos 16 bits baixos e multiplicação inteira de 32 bits, que o DXIL suporta. A extensão depende de Vulkan 1.1 e `VK_KHR_get_physical_device_properties2`, já satisfeitos pela API Vulkan 1.2 do Dozen.

A matriz fixa passa a **28 implementadas e 143 pendentes**, com zero pendentes anunciadas. Um harness gerou módulos SPIR-V não-constantes em todas as larguras permitidas: os 14 opcodes em 32 bits e as dez operações de largura genérica em 8, 16 e 64 bits; `spirv2dxil` compilou **44/44** para DXIL sem capability warning nem opcode NIR não implementado. O teste round35 de 14 módulos encontrou que os dois opcodes de multiplicação 32×16 falhavam antes do novo lowering. Também passaram cinco testes focais, a suíte estática completa (**215 testes**), `compileall`, auditoria do scaffold e build/link Linux. Não houve validação do DXIL em Windows/D3D12 nem Vulkan CTS; não se reivindica conformidade CTS.

## Rodada 37 — outputs de viewport/layer Vulkan 1.2 e alias EXT
Fora da matriz fixa de 171 candidatas, Dozen passa a reportar `shaderOutputViewportIndex` e `shaderOutputLayer` somente quando `D3D12_OPTIONS.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation` está presente. O frontend SPIR-V reconhece as capabilities core independentes `ShaderViewportIndex` e `ShaderLayer` nos estágios vertex/tessellation evaluation/mesh, e o DXIL já mapeia os outputs para `SV_ViewportArrayIndex` e `SV_RenderTargetArrayIndex`. A feature `multiViewport` é reportada com o limite D3D12 `MAX_VP`, usando os arrays existentes de viewport/scissor e os setters de rasterizer D3D12. O alias `VK_EXT_shader_viewport_index_layer` fica disponível apenas com a capacidade D3D12 e `MAX_VP > 1`, respeitando o requisito Vulkan de `multiViewport` e a equivalência com ambas as features core. Um harness compila módulos SPIR-V core e EXT e verifica as assinaturas DXIL, sem substituir validação Windows/D3D12 ou Vulkan CTS. Essa implementação não altera a contagem da matriz: 28 implementadas, 143 pendentes, zero pendentes anunciadas.

## Rodada 38 — `VK_KHR_maintenance11`

Dozen anuncia `VK_KHR_maintenance11` e reporta `maintenance11=true`. A compatibilidade de descritores 1D/2D de uma única camada usa os caminhos D3D12 SRV/UAV existentes, que escolhem a dimensão do descritor pela faixa concreta da view; o runtime comum preserva o flag de criação. O default de depth clipping já corresponde a `!depthClampEnable`. As filas Direct e Compute passam a reportar `minImageTransferGranularity=(1,1,1)` e já executam cópias parciais com `CopyTextureRegion`; criação concorrente com `queueFamilyIndexCount=1` não é rejeitada. Nenhuma fila transfer-only é exposta, então a exceção de alinhamento de `bufferOffset` não tem caso aplicável. `optimalImageTransferGranularity=(0,0,0)` informa, conservadoramente, apenas cópias de mip completo como ótimas. Requisitos condicionais de `VK_EXT_shader_object`, mesh shader e `VK_KHR_extended_flags` não se aplicam porque essas extensões não são anunciadas.

A matriz fixa passa a **29 implementadas e 142 pendentes**, com zero pendentes anunciadas. Passaram sete testes focais, a suíte estática completa (**228 testes**), `compileall`, a auditoria do scaffold e o build/link Linux. Não houve runtime Windows/D3D12 nem Vulkan CTS; não se reivindica conformidade CTS.

## Rodada 39 — `VK_EXT_shader_uniform_buffer_unsized_array`

Dozen anuncia `VK_EXT_shader_uniform_buffer_unsized_array` e reporta `shaderUniformBufferUnsizedArray=true`. O frontend SPIR-V existente representa `OpTypeRuntimeArray` como array sem tamanho em NIR; o caminho UBO do compilador gera acessos `dx.op.cbufferLoadLegacy` com offsets dinâmicos. Um harness SPIR-V válido, com membro prefixo e array final de `vec4` indexado por `VertexIndex`, compilou para DXIL; o limite de uniform buffer permanece o limite D3D12 de 64 KiB. A consulta da estrutura de feature e sua habilitação usam o mecanismo pNext comum gerado a partir do `vk.xml`.

A matriz fixa passa a **30 implementadas e 141 pendentes**, com zero pendentes anunciadas. Passaram seis testes focais novos, a suíte estática completa (**234 testes**), `compileall`, a auditoria do scaffold e build/link Linux do ICD. A faixa CBV é derivada do descritor e a root signature conserva bounds checks D3D12 para buffers. Não houve runtime Windows/D3D12 nem Vulkan CTS; não se reivindica conformidade CTS.

# Provoking vertex refinement (2026-10-07)

`VK_EXT_provoking_vertex` now supports FIRST and LAST modes; the previous
FIRST-only restriction is superseded. Native FIRST/LAST rendering probes and
cache reuse passed for lists, strips, fans, lines, point expansion, application
GS, indexed indirect draws and primitive restart. See README.rpcs3-port.md for
implementation, evidence and validation boundaries. The extension was already
counted as implemented: this refinement does not change the 177/52/125 matrix.

