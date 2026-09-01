# Documentação técnica VaptVupt (pt-BR)

Versão sincronizada com o release **2.65.8**. Este guia resume integração,
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
footer XXH64 opcional. A saída válida de 2.65.8 continua compatível com as
versões anteriores indicadas em [FORMAT.md](FORMAT.md). O campo `window_log`
deve estar entre 10 e 24; valores fora desse intervalo são rejeitados.

No byte de flags, bit 0 indica footer XXH64, bit 1 é reservado e deve ser zero,
bit 2 indica BCJ x86 e bit 3 indica BCJ AArch64; bits 4–7 são reservados.
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
`VV_ERR_NOMEM`, entre outros). No streaming, `dst` deve ser o mesmo endereço
base em todas as chamadas; `written` é cumulativo e `consumed` é por chamada.
Entradas NULL com comprimento não nulo são inválidas. Para embedding simples,
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
alocação e regressões de limites AVX2/SEQ. Execute também ASan+UBSan no
toolchain de destino; ferramentas formais ausentes no host devem ser
reportadas, não tratadas como uma aprovação silenciosa.

Veja [SECURITY.md](SECURITY.md) para o threat model completo e
[INTEGRATION.md](INTEGRATION.md) para recomendações de AEAD, streaming e
multi-thread. Não inclua documentos internos de planejamento ou prompts em
archives de distribuição.
