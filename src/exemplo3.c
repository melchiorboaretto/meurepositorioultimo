/*
 * exemplo3.c - Teste de Esgotamento da Tabela de Arquivos Abertos.
 *
 * Este programa valida a restrição de MAX_OPEN_FILES (10).
 * O sistema deve bloquear a abertura do 11º arquivo simultâneo.
 */

#include <stdio.h>
#include <string.h>
#include "sofs.h"

int main(void)
{
    int particao = 0;
    int setores_por_bloco = 2;

    printf("=== Exemplo 3: Teste de Limite de Arquivos Abertos ===\n");

    if (sofs_format(particao, setores_por_bloco) != 0) {
        fprintf(stderr, "[ERRO] Falha na formatação.\n");
        return 1;
    }
    if (sofs_mount(particao) != 0) {
        fprintf(stderr, "[ERRO] Falha na montagem.\n");
        return 1;
    }

    SOFS_FILE handles[15];
    int abertos_com_sucesso = 0;

    printf("Tentando criar e manter abertos 15 arquivos simultaneamente...\n");
    for (int i = 0; i < 15; i++) {
        char nome[30];
        sprintf(nome, "arquivo_limite_%d.txt", i);
        
        handles[i] = sofs_create(nome);
        if (handles[i] >= 0) {
            abertos_com_sucesso++;
            printf("  [OK] Arquivo '%s' aberto com handle %d\n", nome, handles[i]);
        } else {
            printf("  [BLOQUEADO] Falha ao abrir '%s'. Limite possivelmente atingido.\n", nome);
        }
    }

    printf("\nTotal de arquivos abertos com sucesso: %d\n", abertos_com_sucesso);
    if (abertos_com_sucesso == 10) {
        printf("Resultado: SUCESSO. O sistema limitou corretamente a 10 arquivos.\n");
    } else {
        printf("Resultado: FALHA. O sistema não respeitou o limite MAX_OPEN_FILES = 10.\n");
    }

    /* Liberação obrigatória dos descritores retidos em memória */
    printf("\nFechando todos os arquivos abertos...\n");
    for (int i = 0; i < 15; i++) {
        if (handles[i] >= 0) {
            sofs_close(handles[i]);
        }
    }

    if (sofs_umount() != 0) {
        fprintf(stderr, "[ERRO] Falha na desmontagem.\n");
        return 1;
    }

    printf("Teste concluído.\n");
    return 0;
}
