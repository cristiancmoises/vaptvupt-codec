# VaptVupt

Codec de compressão LZ + tANS em C11, sem dependências de runtime, com
formato aberto e decodificadores de referência byte a byte em Python e
JavaScript. Versão **2.65.8**.

Leia também a [documentação técnica em português](DOCUMENTACAO.pt-BR.md) e a
[documentação normativa em inglês](FORMAT.md). **[English README](README.md)**.

## Situação atual

O release 2.65.8 mantém a saída one-shot compatível e byte-idêntica ao corpus
medido em julho de 2026. A validação de setembro de 2026 confirmou tamanhos e
round-trips; as velocidades abaixo continuam identificadas com o host e a
data originais, sem apresentar números históricos como uma nova medição.

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
Os dados completos e a metodologia estão em [bench/COMPARISON.md](bench/COMPARISON.md).

## Compilar e usar

```sh
make
make test
vaptvupt -c -m balanced -o arquivo.zupt arquivo
vaptvupt -d -o arquivo.out arquivo.zupt
```

`-w N` aceita janelas de 1 KiB a 16 MiB (`10..24`, ou `0` automático).
`--bcj`, `--bcj-arm64` e `--auto-filter` são filtros reversíveis opcionais.
`-A 0` seleciona aceleração automática por modo; `-D N` ajusta a profundidade
do encadeamento. Consulte a [documentação técnica](DOCUMENTACAO.pt-BR.md)
antes de aceitar dados não confiáveis.

## Segurança e integração

O decodificador verifica limites, rejeita frames malformados e nunca substitui
autenticação. O checksum XXH64 detecta corrupção acidental, não adulteração;
use AEAD na camada chamadora. A API de streaming exige o mesmo buffer de saída
estável em todas as chamadas.

O release inclui correções para leitura AVX2 após literais estendidos, entradas
NULL na API de streaming e capacidade/buffer inválidos. `make test` executa
23 suítes C, fuzz diferencial, corpus negativo, testes de DoS, OOM e referências
Python/JavaScript quando Node está disponível.

Licença: GPL-3.0-or-later para a biblioteca. Consulte `NOTICE` e
`LICENSE-COMMERCIAL` para escopo comercial.
