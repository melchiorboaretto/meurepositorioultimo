/*
 * exemplo4.c - Teste de Cadeias de Softlinks e Prevenção de Ciclos.
 *
 * Este programa valida a capacidade do sistema em resolver links
 * simbólicos em cascata e atesta o bloqueio de segurança contra
 * laços infinitos de referências.
 */

#include <stdio.h>
#include <string.h>
#include "sofs.h"

int main(void)
{
    int particao = 0;
    int setores_por_bloco = 2;

    printf("=== Exemplo 4: Cadeias de Softlinks e Ciclos ===\n");

    if (sofs_format(particao, setores_por_bloco) != 0 || sofs_mount(particao) != 0) {
        fprintf(stderr, "[ERRO] Falha na formatação/montagem.\n");
        return 1;
    }

    /* 1. Teste de Resolução em Cascata */
    printf("\n--- Teste A: Resolução em Cascata ---\n");
    SOFS_FILE f_alvo = sofs_create("alvo_real.txt");
    if (f_alvo >= 0) {
        sofs_write(f_alvo, "Dados via Cascata", 17);
        sofs_close(f_alvo);
    }

    printf("Criando estrutura: link_C -> link_B -> link_A -> alvo_real.txt\n");
    sofs_sln("link_A", "alvo_real.txt");
    sofs_sln("link_B", "link_A");
    sofs_sln("link_C", "link_B");

    SOFS_FILE f_leitura = sofs_open("link_C");
    if (f_leitura >= 0) {
        char buffer[30];
        memset(buffer, 0, sizeof(buffer));
        int lidos = sofs_read(f_leitura, buffer, 17);
        printf("Leitura efetuada a partir de 'link_C' (%d bytes): %s\n", lidos, buffer);
        sofs_close(f_leitura);
    } else {
        printf("[ERRO] O sistema falhou ao resolver a cadeia de softlinks lícita.\n");
    }

    /* 2. Teste de Ciclo Infinito */
    printf("\n--- Teste B: Prevenção de Ciclo Infinito ---\n");
    printf("Criando ciclo estrutural fechado: loop1 -> loop2 -> loop1\n");
    sofs_sln("loop1", "loop2");
    sofs_sln("loop2", "loop1");

    printf("Tentando abrir 'loop1'...\n");
    SOFS_FILE f_loop = sofs_open("loop1");
    if (f_loop < 0) {
        printf("Resultado: SUCESSO. A abertura foi abortada pelo sistema, prevenindo o ciclo infinito.\n");
    } else {
        printf("Resultado: FALHA CRÍTICA. O sistema permitiu a abertura de um link cíclico.\n");
        sofs_close(f_loop);
    }

    sofs_umount();
    printf("\nTeste concluído.\n");
    return 0;
}
