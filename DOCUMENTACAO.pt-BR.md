# Documentação técnica VaptVupt (pt-BR)

Versão sincronizada com o release **2.65.10**. Este guia resume integração,
formato e limites de segurança em português; os documentos em inglês
[FORMAT.md](FORMAT.md), [SECURITY.md](SECURITY.md) e [INTEGRATION.md](INTEGRATION.md)
são as referências normativas completas. **[README em português](README.pt-BR.md)**
· **[English README](README.md)**.

## Limite de segurança

Trate todo frame recebido como não confiável. `vv_decompress()` e
`vv_dstream_decompress_chunk()` devem retornar erro limpo, sucesso ou fim de
frame; nunca confie no conteúdo para dimensionar memória sem impor `dst_cap`.
XXH64 é somente detecção de corrupção acidental. Para confidencialidade e
autenticidade, envolva o frame em AEAD (por exemplo, AES-GCM ou
ChaCha20-Poly1305) e não misture segredos com texto controlado pelo atacante.

O codec não protege contra exaustão de CPU/memória causada por codificação
`extreme`, concorrência do chamador, swap, dumps do sistema ou ataques de
side-channel. A aplicação deve impor limites de tamanho, tempo e processos.

## Formato e compatibilidade

Um frame contém cabeçalho de 16 bytes, blocos RAW/RLE/token/entropia e um
footer XXH64 opcional. A saída válida de 2.65.10 continua compatível com as
versões anteriores indicadas em [FORMAT.md](FORMAT.md). O campo `window_log`
deve estar entre 10 e 24; valores fora desse intervalo são rejeitados.
Desde v2.65.10, offsets também precisam respeitar a janela declarada, e toda
extensão de comprimento de token precisa de seu byte terminador, inclusive
quando a extensão vale zero.

No byte de flags, bit 0 indica footer XXH64, bit 1 é reservado e deve ser zero,
bit 2 indica BCJ x86 e bit 3 indica BCJ AArch64; bits 4–7 são reservados. Os
bits BCJ x86 e AArch64 são mutuamente exclusivos em uma saída válida. No
one-shot, no streaming e em `vv_get_frame_info`, o decodificador rejeita um
cabeçalho com ambos ligados. No streaming, a inversa escolhida é aplicada uma
única vez sobre o frame completo, depois da validação do checksum ou, sem
checksum, depois do bloco final.
Offsets usam 2 bytes até `window_log=16` e 3 bytes até 24. O formato v2 (`T`)
é selecionado automaticamente para dados binários em balanced/extreme; use
`compat_v246_5_decoder` quando o consumidor precisar de decodificadores antigos.

## API mínima

```c
vv_options_t opt;
vv_default_options(&opt);
opt.mode = VV_MODE_BALANCED;
int64_t n = vv_compress(src, src_len, dst, vv_compress_bound(src_len), &opt);
int64_t m = vv_decompress(dst, (size_t)n, out, out_cap);
```

Retorno negativo é erro (`VV_ERR_OVERFLOW`, `VV_ERR_CORRUPT`, `VV_ERR_PARAM`,
`VV_ERR_NOMEM`, entre outros). `vv_compress` retorna `VV_ERR_PARAM` para um
modo fora de `VV_MODE_ULTRA_FAST`, `VV_MODE_BALANCED` e `VV_MODE_EXTREME`, ou
quando `filter_x86` e `filter_arm64` são pedidos simultaneamente. O encoder de
streaming não pode aplicar um filtro de frame inteiro: `vv_cstream_create`
retorna NULL e `vv_cstream_reset` retorna `VV_ERR_PARAM` para modo inválido ou
qualquer opção BCJ; use `vv_compress` quando precisar do filtro. Na
descompressão streaming, `dst` deve ser o mesmo endereço
base em todas as chamadas; `written` é cumulativo e `consumed` é por chamada.
Isso inclui chamadas feitas após o estado de conclusão do decodificador.
`vv_cstream_reset` aplica as restrições de comprimento e o matcher hash3 do
novo `format_v2`; alternar v1/v2 agora equivale a criar um contexto novo,
corrigindo o caso de corrupção com matches longos. Se a alocação necessária
falhar, retorna `VV_ERR_NOMEM` antes de aceitar as novas opções.
Uma janela fora de 10..24 é rejeitada antes de qualquer cópia/transformação BCJ.
Entradas NULL com comprimento não nulo são inválidas. Em frames BCJ, não
publique os bytes parciais antes do retorno de conclusão: eles só recebem a
inversa no fim do frame. Para embedding simples,
`make amalg` gera `build/vaptvupt.c` e `build/vaptvupt.h`; valide com
`make amalg-verify`.

## Verificação do release

```sh
make
make check-debug
make test
make amalg-verify
```

O conjunto inclui round-trip em todos os modos, fuzz diferencial C↔Python,
referências JavaScript, corpus negativo, reprodutores de DoS, falhas de
alocação e regressões de limites AVX2/SEQ. No v2.65.9, também compara a nova
construção direta das tabelas tANS com a construção histórica e cobre a inversa
BCJ no streaming em frames inteiros/divididos, x86/AArch64 e checksum
ligado/desligado. A tabela direta reduz o scratch das tabelas de sequência por
bloco de 52 para 48 KiB sem alterar a saída no fio. Execute também ASan+UBSan no
toolchain de destino; ferramentas formais ausentes no host devem ser
reportadas, não tratadas como uma aprovação silenciosa.

O v2.65.10 inclui regressões para as transições de formato no reset, extensões
sem terminador, offsets além da janela e `written` após a conclusão. A escrita
de bits Huffman evita shift indefinido e overflow, bitstreams truncados são
rejeitados, e a decodificação ANS valida que a soma das frequências normalizadas
preenche exatamente a tabela. A CLI exige argumentos numéricos completos e
modos válidos, detecta falha de flush/fechamento da saída e tem testes de
contrato. `make test` propaga todas as falhas dos subcomandos Python.

O harness formal do leitor `read_ext_len` acompanha a nova rejeição de
terminadores ausentes, mas CBMC não estava disponível. A prova histórica desse
helper não certifica o código alterado; a evidência atual é dinâmica, com
regressões e sanitizers. Consulte [verification/README.md](verification/README.md)
para os helpers e limites cobertos historicamente.

O layout SEQ contém um `match_count` global: apenas entradas LL depois de todos
os matches podem ser sem match. Por isso, um literal acima de 65.535 bytes antes
de um match posterior não pode ser dividido no meio. O v2.65.9 rejeita essa
candidata SEQ e usa um bloco alternativo sem perda; `test_seq_v2` valida o caso
direto e um reproducer end-to-end determinístico (21/21). Python e JavaScript
agora consomem entradas LL finais com o limite de iterações equivalente ao de
C, rejeitam flags BCJ duplas e aplicam as inversas exatas x86/AArch64 depois da
validação do checksum. Fixtures checksum on/off confirmam a saída atual byte a
byte. C permanece canônico para o conjunto legado H/A/I/C; Python mantém suporte
limitado a A, enquanto JavaScript omite essas tags antigas. O sweep OOM confirma
o round-trip do fixture-base antes da injeção.
A cópia privada de entrada do BCJ one-shot é zerada explicitamente antes de
`free()`. O teste associado cobre a execução e o round-trip sob sanitizers; não
é uma prova direta do conteúdo da memória depois de liberada.

Para a comparação do release, `bench/competitive.py --generated-suite` cria a
suite determinística `generated-v1`, exige por padrão a matriz completa
vv-fast/vv-balanced/lz4-1/zstd-1/zstd-3 e verifica cada decodificação por
SHA-256. Use `--runs 7 --warmups 1 --csv ... --json ...`; o JSON registra a
proveniência do host e das ferramentas, e o CSV registra hashes e medições.
As tabelas comparativas atuais medem v2.65.10 em 06/09/2026. Separadamente,
o microbenchmark interno de alocação do v2.65.10 contra v2.65.9 mediu +32,3%
em texto fast de 1 KiB e +17,2%/+39,6% em dados aleatórios de 1 MiB nos modos
fast/balanced, com saída comprimida idêntica. A remoção do hash4 não utilizado
evita solicitar 512 KiB por matcher na janela padrão, até 64,25 MiB na maior;
isso não mede redução de RSS. Metodologia e limites em
[bench/COMPARISON.md](bench/COMPARISON.md).

Veja [SECURITY.md](SECURITY.md) para o threat model completo e
[INTEGRATION.md](INTEGRATION.md) para recomendações de AEAD, streaming e
multi-thread. O artefato de distribuição atual é somente o arquivo-fonte
`vaptvupt-2.65.10-src.tar.gz`; não inclua material interno no archive.
