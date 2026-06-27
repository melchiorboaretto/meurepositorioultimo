/*
 * exemplo5.c - Teste de Persistência de Dados e Deleção Segura via Hardlinks.
 *
 * Este programa demonstra que os blocos de dados originais são mantidos
 * intactos enquanto houver pelo menos um hardlink apontando para o i-node,
 * confirmando o funcionamento do contador de referências (RefCounter).
 */

#include <stdio.h>
#include <string.h>
#include "sofs.h"

int main(void)
{
    int particao = 0;
    int setores_por_bloco = 2;

    printf("=== Exemplo 5: Persistência de Dados via Hardlinks ===\n");

    if (sofs_format(particao, setores_por_bloco) != 0 || sofs_mount(particao) != 0) {
        fprintf(stderr, "[ERRO] Falha na formatação/montagem.\n");
        return 1;
    }

    printf("\n1. Criando arquivo original 'base.txt'...\n");
    SOFS_FILE f_base = sofs_create("base.txt");
    if (f_base >= 0) {
        sofs_write(f_base, "Dados Persistentes", 18);
        sofs_close(f_base);
    }

    printf("2. Criando dois hardlinks para 'base.txt' ('hlink_1' e 'hlink_2')...\n");
    sofs_hln("hlink_1", "base.txt");
    sofs_hln("hlink_2", "base.txt");

    printf("3. Deletando o arquivo original 'base.txt'...\n");
    sofs_delete("base.txt");

    printf("4. Tentando ler dados através de 'hlink_1'...\n");
    SOFS_FILE h_open1 = sofs_open("hlink_1");
    if (h_open1 >= 0) {
        char buffer1[30];
        memset(buffer1, 0, sizeof(buffer1));
        sofs_read(h_open1, buffer1, 18);
        printf("   Leitura via 'hlink_1': %s\n", buffer1);
        sofs_close(h_open1);
    } else {
        printf("   [ERRO] Falha ao abrir 'hlink_1'. Dados podem ter sido destruídos prematuramente.\n");
    }

    printf("5. Deletando 'hlink_1'...\n");
    sofs_delete("hlink_1");

    printf("6. Tentando ler dados remanescentes através de 'hlink_2'...\n");
    SOFS_FILE h_open2 = sofs_open("hlink_2");
    if (h_open2 >= 0) {
        char buffer2[30];
        memset(buffer2, 0, sizeof(buffer2));
        sofs_read(h_open2, buffer2, 18);
        printf("   Leitura via 'hlink_2': %s\n", buffer2);
        sofs_close(h_open2);
    } else {
        printf("   [ERRO] Falha ao abrir 'hlink_2'.\n");
    }

    printf("7. Deletando o último hardlink 'hlink_2' (Blocos devem ser liberados fisicamente do disco).\n");
    sofs_delete("hlink_2");

    sofs_umount();
    printf("\nTeste concluído com sucesso.\n");
    return 0;
}
