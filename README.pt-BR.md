# VaptVupt

Codec de compressão LZ + tANS em C11, sem dependências de runtime, com
formato aberto e decodificadores de referência em Python e JavaScript que
reproduzem byte a byte a saída atual/padrão do encoder. Versão **2.65.9**.

Leia também a [documentação técnica em português](DOCUMENTACAO.pt-BR.md) e a
[documentação normativa em inglês](FORMAT.md). **[English README](README.md)**.

## Situação atual

### Suite determinística generated-v1 do v2.65.9 (medida em 01/09/2026)

Medição nova por subprocesso em Intel Core i7-13700HX, Linux 7.2.2 e gcc 14.3,
fixada no core 2. Cada célula é a mediana de 7 execuções após 1 aquecimento;
zstd 1.5.6 usou uma thread e lz4 é 1.10. Todas as saídas decodificadas foram
verificadas por SHA-256. As células mostram
`razão @ compressão/decodificação MB/s`.

| file | vv-fast | vv-balanced | lz4-1 | zstd-1 | zstd-3 |
|---|---|---|---|---|---|
| text.txt | 3.707 @134.3/396.0 | 7.055 @56.7/327.5 | 2.907 @274.5/379.0 | 5.768 @194.7/343.3 | 6.198 @175.1/342.9 |
| records.jsonl | 3.142 @128.9/420.8 | 5.707 @48.8/341.7 | 3.505 @272.5/370.9 | 7.071 @215.1/339.8 | 6.521 @175.0/334.0 |
| records.bin | 1.347 @72.3/405.1 | 2.016 @17.3/229.9 | 1.371 @251.4/384.6 | 1.934 @180.1/317.5 | 2.183 @116.1/269.6 |
| random.bin | 1.000 @350.0/434.7 | 1.000 @239.4/402.1 | 1.000 @364.3/362.9 | 1.000 @312.6/334.4 | 1.000 @274.3/321.2 |

O resultado é modesto e depende da carga, não estabelece uma ordem universal.
Nesta suite, `vv-balanced` obtém a melhor razão em texto e pequena vantagem de
decodificação sobre zstd-1/3 no JSON gerado, mas zstd comprime esses dados muito
mais rápido e tem razão melhor no JSON. `vv-fast` supera lz4-1 em razão no
texto, mas não no JSON nem nos registros binários; lz4 comprime mais rápido.
Meça os dados reais.

Para reproduzir, coloque lz4 1.10 no início de `PATH`:

```sh
PATH=/caminho/para/lz4-1.10/bin:$PATH taskset -c 2 \
  python3 bench/competitive.py --generated-suite --vv ./vaptvupt \
  --runs 7 --warmups 1 --csv generated-v1.csv --json generated-v1.json
```

### Recorte do corpus histórico de 11 arquivos (medido em 2026-07; revalidado em 2026-09)

O release 2.65.9 mantém a saída one-shot compatível e byte-idêntica ao corpus
medido em julho de 2026. A validação de setembro de 2026 confirmou tamanhos e
round-trips; as velocidades abaixo continuam identificadas com o host e a
data originais, sem apresentar números históricos como uma nova medição. Esta
tabela complementa, sem substituir, a suite generated-v1 acima.

Este recorte e a tabela completa em [bench/COMPARISON.md](bench/COMPARISON.md)
registram execuções de tempo distintas de julho dentro da mesma família de
benchmark: as razões coincidem, mas cada documento preserva suas próprias
velocidades. O guia de comparação contém o corpus completo e a metodologia.

| arquivo | vv-balanced | vv-extreme | zstd-3 | zstd-9 | lz4-1 |
|---|---|---|---|---|---|
| access.log | 6,322 @46/375 | 8,045 @2/459 | 6,496 @241/329 | 8,238 @58/551 | 4,070 @195/209 |
| data.json | **5,435 @68/552** | **6,078 @2/484** | 5,241 @150/603 | 5,801 @62/674 | 2,889 @329/216 |
| table.csv | **3,210 @26/320** | 3,728 @1/290 | 3,189 @117/292 | 3,811 @35/318 | 2,053 @242/366 |
| catalog.xml | 11,195 @51/382 | **13,559 @2/505** | 11,897 @205/228 | 12,563 @63/452 | — |
| text.md | 2,772 @29/297 | 2,812 @5/204 | 2,872 @40/199 | 3,146 @26/195 | — |
| sensors.bin | **1,398 @13/237** | — | 1,176 @119/134 | — | — |
| structs.bin | **1,719 @13/314** | — | 1,595 @86/250 | 1,600 @41/268 | — |

Cada célula é `razão @ compressão/decodificação MB/s`; razão maior é melhor.

No v2.65.9, as tabelas tANS de decodificação de sequências são construídas
diretamente, eliminando 4 KiB do scratch das tabelas de sequência por bloco
(52 para 48 KiB). Uma medição pareada, in-process e com CPU fixada observou
+0,40% em texto e +1,21% em JSON
(cerca de +0,80% de média geométrica); é um ganho pequeno e dependente da
carga, com saída no fio inalterada.

## Compilar e usar

```sh
make
make test
vaptvupt -c -m balanced -o arquivo.zupt arquivo
vaptvupt -d -o arquivo.out arquivo.zupt
```

`-w N` aceita janelas de 1 KiB a 16 MiB (`10..24`, ou `0` automático).
`--bcj`, `--bcj-arm64` e `--auto-filter` são filtros reversíveis opcionais;
x86 e ARM64 são mutuamente exclusivos, e o decodificador rejeita um frame que
marque ambos. `-A 0` seleciona aceleração automática (fast=2,
balanced/extreme=1); `-D N` ajusta a profundidade
do encadeamento. Consulte a [documentação técnica](DOCUMENTACAO.pt-BR.md)
antes de aceitar dados não confiáveis.

## Segurança e integração

O decodificador verifica limites, rejeita frames malformados e nunca substitui
autenticação. O checksum XXH64 detecta corrupção acidental, não adulteração;
use AEAD na camada chamadora. A API de streaming exige o mesmo buffer de saída
estável em todas as chamadas.

No streaming, o v2.65.9 aplica a inversa BCJ exatamente uma vez no fim do frame:
depois de validar o checksum, ou logo após o último bloco quando não há
checksum. Há regressões de frame inteiro e dividido para x86/ARM64 com checksum
ligado e desligado. `vv_compress` rejeita modos fora do enum e filtros x86 e
ARM64 simultâneos com `VV_ERR_PARAM`. O encoder de streaming rejeita modos
inválidos e qualquer opção BCJ, que exige o caminho one-shot, em vez de ignorar
o filtro silenciosamente. `make test` também verifica a equivalência
das tabelas tANS diretas, além do fuzz diferencial, corpus negativo, testes de
DoS/OOM e referências Python/JavaScript quando Node está disponível. Uma
candidata SEQ com literal não terminal acima de 65.535 bytes agora é rejeitada
em favor de um bloco alternativo sem perda, em vez de gerar frame
indecodificável; `test_seq_v2` cobre o caso direto e o round-trip (21/21). O
Python e JavaScript agora consomem entradas LL finais com o mesmo limite de
iterações de C, evitando término prematuro e loops em entrada corrompida. As
duas referências rejeitam cabeçalhos BCJ duplos e aplicam as inversas exatas
x86/AArch64 depois do checksum; fixtures checksum on/off confirmam a saída
atual byte a byte. C continua canônico para o conjunto legado H/A/I/C; Python
mantém suporte limitado a A, enquanto JavaScript omite essas tags antigas. O
sweep OOM valida o round-trip-base antes de injetar falhas. A cópia privada de entrada
usada pelo BCJ one-shot agora é
zerada explicitamente antes de `free()`; `test_secure_zero` cobre a conclusão
desse caminho e o round-trip sob sanitizers, sem alegar inspeção da memória
depois de liberada.

Licença: GPL-3.0-or-later para a biblioteca. Consulte `NOTICE` e
`LICENSE-COMMERCIAL` para escopo comercial.
