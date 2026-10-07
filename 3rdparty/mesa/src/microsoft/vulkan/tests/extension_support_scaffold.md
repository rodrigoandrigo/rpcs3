# Base de implementação Vulkan do Dozen — Mesa 26.2.2

A base registra **136 extensões pendentes** sem anunciá-las. Ela converte os requisitos do `vk.xml` em contratos de trabalho verificáveis; não representa suporte runtime.

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

- Candidatas revisadas: 171
- Implementadas na matriz: 35
- Pendentes com checklist por extensão: 136
- Pendentes que aparecem anunciadas na fonte: 0
- Extensões não encontradas no `vk.xml`: 0

## Bloqueios registrados

- `feature_and_zero_one_semantics_unimplemented`: 1
- `feature_not_reported`: 64
- `missing_backend_commands`: 53
- `unhandled_type_or_semantics`: 17
- `unsupported_stub_and_feature_false`: 1

## Regra para promover uma extensão

Uma extensão só pode sair da pendência depois que probe de capacidade, feature/property, dependências, estruturas `pNext`, comandos, tradução D3D12/compilador e testes aplicáveis estiverem cobertos. Símbolos gerados ou handlers comuns, isoladamente, não provam que a semântica esteja implementada.

A lista completa e os requisitos de cada extensão estão em `extension_support_scaffold.json`. O teste `test_extension_support_scaffold.py` falha se uma extensão pendente for anunciada sem atualizar a matriz e comprovar sua implementação.

