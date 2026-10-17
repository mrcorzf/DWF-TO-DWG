# Conversor DWF 3D para DWG / DXF

Conversor local para **Windows 10 (1903 ou posterior) / Windows 11, x64**.
Lê modelos W3D dentro de pacotes DWF e grava geometria 3D em DWG 2000
(AC1015) ou DXF 2000. Não exige AutoCAD instalado nem envia desenhos à nuvem.

## Usar no Windows

1. Extraia o pacote completo para uma pasta. Mantenha `dwf3d.exe` e
   `dwgwriter.exe` juntos.
2. Abra `dwf3d.exe` e selecione um DWF. O DWG será criado ao lado dele.
3. Abra o DWG no seu CAD e use Zoom Extents / Zoom em tudo.

Pelo PowerShell, para escolher nome e formato:

```powershell
.\dwf3d.exe "C:\Desenhos\F-2151C.dwf" "C:\Desenhos\F-2151C.dwg"
.\dwf3d.exe "C:\Desenhos\F-2151C.dwf" "C:\Desenhos\F-2151C.dxf"
```

Não sobrescreve arquivos existentes. Para repetir uma conversão, escolha outro
nome de saída. `dwgwriter.exe` é um componente necessário para gerar DWG;
a saída DXF é produzida diretamente pelo `dwf3d.exe`.

## Escopo e precisão

- Implementado para pacotes com exatamente um modelo W3D: malhas com faces
  triangulares/quadrangulares, triangle strips, círculos e cilindros.
- Aplica as matrizes da hierarquia e mantém as unidades declaradas no DWF.
- Malhas viram entidades `3DFACE`, separadas em camadas `MALHA_n`.
- Círculos permanecem `CIRCLE` em seus planos 3D. Cilindros viram malhas
  com 128 segmentos por seção e tampas conforme o original. O erro radial
  máximo local é `raio × (1 - cos(pi/128))`, aproximadamente 0,0302% do raio;
  escalas do modelo também afetam esse erro.
- Não recupera sólidos ACIS/paramétricos, histórico de modelagem, materiais,
  cores, texturas, visibilidade, nomes originais ou propriedades BIM.
- Referências externas, instâncias, LODs, NURBS, texto e outras primitivas
  ainda não implementadas interrompem a conversão com erro. Não são
  omitidas silenciosamente. Arquivos DWF 2D e DWFx não são suportados.
- Círculos sob escala não uniforme, polígonos com mais de quatro vértices,
  faces com furos e transformações não triviais no descritor são recusados.
- Limites: entrada/recurso de até 256 MiB, 10 milhões de vértices e
  2 milhões de faces. Não é um conversor universal de todos os DWF.

## Validação do exemplo F-2151C.dwf

Arquivo de origem: Navisworks Manage 2027, DWF 6.01 / W3D HSF 12.15,
unidades em metros. SHA-256:
`4c7e6a63735a2aba5cbdbf3660f2e281cafd1f56f37fafa271fa351f01a2665e`.

Leitura completa: 626 malhas originais, 256 cilindros e 512 círculos.
Resultado: **57.000 entidades 3DFACE e 512 entidades CIRCLE**.
O gravador relê o DWG e compara todas as coordenadas, raios, normais e
unidades com a entrada DXF, com tolerância absoluta de `1e-9` unidades.

Os testes de conversão foram executados no Linux. Os executáveis Windows
são compilados para x64; a execução e a interface no Windows, assim como
a abertura no AutoCAD/Navisworks, ainda precisam de validação nesse sistema.

## Compilar a partir do código

Host de compilação: Linux, Python 3.11+, Git, GCC/G++, Make, CMake e Perl.
Para o alvo Windows, use também **Zig 0.13.0**. Dependências são obtidas por
HTTPS e fixadas em commits, sem alterar arquivos das bibliotecas.

```bash
python3 tools/build.py --target linux --jobs 4
python3 tools/build.py --target windows --zig /caminho/zig --jobs 3
```

Saídas em `dist/linux/` ou `dist/windows/`; logs e dependências em `build/`.
O script aceita `--cmake`, `--toolkit`, `--libredwg` e `--zlib` para reutilizar
instalações/checkouts dos mesmos commits. Não cria worktrees.

Para testar o exemplo, mantendo-o fora do controle de versão:

```bash
python3 -m pip install -r requirements-dev.txt
python3 tests/test_conversion.py --fixture /caminho/F-2151C.dwf
```

No Linux, o processo equivalente é:

```bash
dist/linux/dwf3d /caminho/modelo.dwf /tmp/modelo.dxf
dist/linux/dwgwriter /tmp/modelo.dxf /tmp/modelo.dwg
```

## Componentes e licenças

`dwf3d` usa o Autodesk DWF Toolkit e zlib; seu frontend novo tem licença MIT.
`dwgwriter` é um programa separado, sob GPLv3+, que usa LibreDWG. A comunicação
entre os programas ocorre por arquivo DXF; não há ligação entre as bibliotecas
Autodesk e LibreDWG. Licenças dos componentes acompanham a distribuição.
Os commits exatos estão em `tools/build.py` e no `VERSIONS.json` do pacote.
Ao redistribuir o gravador, forneça também seu código e as fontes correspondentes
do LibreDWG, conforme a GPL. O pacote de fontes contém as dependências utilizadas.
