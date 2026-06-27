/*
 * sofs.c - Implementação (esqueleto) do sistema de arquivos sofs.
 *
 * A camada de blocos (sofs-block) é usada para todos os acessos ao disco;
 * a camada de bitmap (bitmap2) gerencia o controle de blocos e i-nodes livres.
 *
 * Layout do sistema de arquivos dentro de uma partição (em ordem):
 * [bloco 0]          superbloco
 * [blocos 1 .. bb]   bitmap de blocos livres   (bb = freeBlocksBitmapSize)
 * [bb+1 .. bb+bi]    bitmap de i-nodes livres  (bi = freeInodeBitmapSize)
 * [bb+bi+1 .. ...]   área de i-nodes           (10% dos blocos, arredondado para cima)
 * [resto]            blocos de dados
 *
 * As funções marcadas com TODO são responsabilidade do grupo.
 * As funções auxiliares alloc_data_block(), free_data_block(),
 * alloc_inode() e free_inode() são fornecidas como blocos de construção.
 */

#include <string.h>
#include "sofs.h"
#include "sofs-block.h"

/* -------------------------------------------------------------------------
 * Estado interno de montagem
 * ---------------------------------------------------------------------- */

static int g_mounted = false;
static struct sofs_superbloco g_superbloco;
static unsigned int g_superbloco_sector;   /* setor absoluto do superbloco */

/* Tabela de Arquivos Abertos (Capacidade para 10 arquivos abertos simultaneamente) */
#define MAX_OPEN_FILES 10
struct open_file_entry {
    int in_use;             /* 0 se livre, 1 se ocupado */
    unsigned int inode_num; /* Índice do i-node referenciado */
    unsigned int offset;    /* Ponteiro de posição corrente (bytes) */
};
static struct open_file_entry g_open_files[MAX_OPEN_FILES];

/* Controle iterador para o diretório raiz */
static unsigned int g_dir_offset = 0;

/* --- Protótipos das funções auxiliares adicionais --- */
static int read_inode(unsigned int inode_num, struct sofs_inode *inode);
static int write_inode(unsigned int inode_num, struct sofs_inode *inode);
static int get_block_from_inode(struct sofs_inode *inode, unsigned int logical_block);
static int allocate_block_for_inode(struct sofs_inode *inode, unsigned int logical_block);
static void free_inode_blocks(struct sofs_inode *inode);
static int find_dir_entry(const char *name, struct sofs_record *out_rec, unsigned int *out_offset);
static int add_dir_entry(const char *name, unsigned int inode_num, BYTE typeVal);
static int remove_dir_entry(unsigned int offset);

/* -------------------------------------------------------------------------
 * Auxiliar: lê o MBR e localiza a partição <partition>.
 * Preenche *first_sector e *num_sectors.
 * Retorna 0 em caso de sucesso.
 * ---------------------------------------------------------------------- */
static int read_partition_info(int partition,
                               unsigned int *first_sector,
                               unsigned int *num_sectors)
{
    unsigned char mbr_buf[SECTOR_SIZE];
    struct sofs_mbr *mbr;

    if (read_sector(0, mbr_buf) != 0)
        return -1;

    mbr = (struct sofs_mbr *)mbr_buf;

    if (partition < 0 || partition >= (int)mbr->numPartitions)
        return -1;

    *first_sector = mbr->partitionTable[partition].firstSector;
    *num_sectors  = mbr->partitionTable[partition].lastSector
                    - mbr->partitionTable[partition].firstSector + 1;
    return 0;
}

/* -------------------------------------------------------------------------
 * Funções básicas de criação/destruição de blocos de dados e i-nodes.
 *
 * Fornecidas como blocos de construção para a implementação do grupo em
 * sofs_create, sofs_delete, sofs_read, sofs_write, etc.
 * ---------------------------------------------------------------------- */

/*
 * alloc_data_block - aloca o primeiro bloco de dados livre.
 *
 * Pesquisa no bitmap de dados o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do bloco e retorna o número absoluto do bloco na partição.
 *
 * Retorna o número do bloco (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se o disco estiver cheio.
 */
static int alloc_data_block(void)
{
    int bit;
    unsigned int block_size;
    unsigned char *buf;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_DADOS, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_DADOS, bit, 1) != 0)
        return -1;

    /* Inicializa o bloco recém-alocado com zeros */
    block_size = g_superbloco.blockSize * SECTOR_SIZE;
    buf = (unsigned char *)__builtin_alloca(block_size);
    memset(buf, 0, block_size);

    /* O primeiro bloco de dados começa após superbloco + bitmaps + área de i-nodes */
    unsigned int first_data_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;

    if (write_block(first_data_block + (unsigned int)bit, buf) != 0) {
        setBitmap2(BITMAP_DADOS, bit, 0);
        return -1;
    }

    return (int)(first_data_block + (unsigned int)bit);
}

/*
 * free_data_block - libera um bloco de dados previamente alocado.
 *
 * abs_block_num : número absoluto do bloco na partição (conforme
 * retornado por alloc_data_block).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_data_block(unsigned int abs_block_num)
{
    unsigned int first_data_block;
    int bit;

    if (!g_mounted)
        return -1;

    first_data_block = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + g_superbloco.inodeAreaSize;

    if (abs_block_num < first_data_block)
        return -1;

    bit = (int)(abs_block_num - first_data_block);
    return setBitmap2(BITMAP_DADOS, bit, 0);
}

/*
 * alloc_inode - aloca o primeiro i-node livre.
 *
 * Pesquisa no bitmap de i-nodes o primeiro bit livre, marca-o como ocupado,
 * zera o conteúdo do i-node em disco e retorna o número do i-node.
 *
 * Retorna o número do i-node (>= 0) em caso de sucesso; -1 em caso de erro
 * ou se todos os i-nodes estiverem em uso.
 */
static int alloc_inode(void)
{
    int bit;
    unsigned int inode_block;
    unsigned int inodes_per_block;
    unsigned int inode_offset;
    unsigned char *buf;
    unsigned int block_size;

    if (!g_mounted)
        return -1;

    bit = searchBitmap2(BITMAP_INODE, 0);
    if (bit < 0)
        return -1;

    if (setBitmap2(BITMAP_INODE, bit, 1) != 0)
        return -1;

    /* Zera o i-node em disco */
    block_size     = g_superbloco.blockSize * SECTOR_SIZE;
    inodes_per_block = block_size / sizeof(struct sofs_inode);
    inode_block    = 1
        + g_superbloco.freeBlocksBitmapSize
        + g_superbloco.freeInodeBitmapSize
        + (unsigned int)bit / inodes_per_block;
    inode_offset   = (unsigned int)bit % inodes_per_block;

    buf = (unsigned char *)__builtin_alloca(block_size);
    if (read_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    memset(buf + inode_offset * sizeof(struct sofs_inode), 0,
           sizeof(struct sofs_inode));

    if (write_block(inode_block, buf) != 0) {
        setBitmap2(BITMAP_INODE, bit, 0);
        return -1;
    }

    return bit;
}

/*
 * free_inode - libera um i-node previamente alocado.
 *
 * inode_num : número do i-node (conforme retornado por alloc_inode).
 *
 * Retorna 0 em caso de sucesso; -1 em caso de erro.
 */
static int free_inode(unsigned int inode_num)
{
    if (!g_mounted)
        return -1;

    return setBitmap2(BITMAP_INODE, (int)inode_num, 0);
}

/* -------------------------------------------------------------------------
 * Funções Auxiliares de manipulação Estrutural (Inodes/Diretório) 
 * - Elas são usadas no TODO, mas deixamos separadas para visualização mais fácil e maior organização
 * ---------------------------------------------------------------------- */

/* Lê fisicamente um i-node específico do disco para uma estrutura em memória */
static int read_inode(unsigned int inode_num, struct sofs_inode *inode) {
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int inodes_per_block = block_size / sizeof(struct sofs_inode);
    unsigned int inode_block = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize + (inode_num / inodes_per_block);
    unsigned int inode_offset = inode_num % inodes_per_block;

    unsigned char buf[block_size];
    if (read_block(inode_block, buf) != 0) return -1;
    memcpy(inode, buf + inode_offset * sizeof(struct sofs_inode), sizeof(struct sofs_inode));
    return 0;
}

/* Grava fisicamente um i-node específico da memória para o disco */
static int write_inode(unsigned int inode_num, struct sofs_inode *inode) {
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int inodes_per_block = block_size / sizeof(struct sofs_inode);
    unsigned int inode_block = 1 + g_superbloco.freeBlocksBitmapSize + g_superbloco.freeInodeBitmapSize + (inode_num / inodes_per_block);
    unsigned int inode_offset = inode_num % inodes_per_block;

    unsigned char buf[block_size];
    if (read_block(inode_block, buf) != 0) return -1;
    memcpy(buf + inode_offset * sizeof(struct sofs_inode), inode, sizeof(struct sofs_inode));
    return write_block(inode_block, buf);
}

/* Resolve e retorna o número absoluto do bloco físico a partir de um bloco lógico (índice 0, 1, 2...) */
static int get_block_from_inode(struct sofs_inode *inode, unsigned int logical_block) {
    /* Lógica para ponteiros diretos (0 e 1) */
    if (logical_block < 2) {
        return inode->dataPtr[logical_block];
    }
    
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int ptrs_per_block = block_size / sizeof(DWORD);
    
    /* Lógica para indireção simples */
    if (logical_block < 2 + ptrs_per_block) {
        if (inode->singleIndPtr == 0) return 0;
        unsigned char buf[block_size];
        if (read_block(inode->singleIndPtr, buf) != 0) return -1;
        DWORD *ptrs = (DWORD *)buf;
        return ptrs[logical_block - 2];
    }
    
    /* Lógica para indireção dupla implementada para suportar arquivos maiores */
    else if (logical_block < 2 + ptrs_per_block + (ptrs_per_block * ptrs_per_block)) {
        if (inode->doubleIndPtr == 0) return 0;
        unsigned char buf1[block_size];
        if (read_block(inode->doubleIndPtr, buf1) != 0) return -1;
        DWORD *ptrs1 = (DWORD *)buf1;
        
        unsigned int double_idx = logical_block - 2 - ptrs_per_block;
        unsigned int idx1 = double_idx / ptrs_per_block;
        unsigned int idx2 = double_idx % ptrs_per_block;
        
        if (ptrs1[idx1] == 0) return 0;
        
        unsigned char buf2[block_size];
        if (read_block(ptrs1[idx1], buf2) != 0) return -1;
        DWORD *ptrs2 = (DWORD *)buf2;
        return ptrs2[idx2];
    }
    
    return 0; 
}

/* Aloca um novo bloco físico e o mapeia no índice lógico do respectivo i-node */
static int allocate_block_for_inode(struct sofs_inode *inode, unsigned int logical_block) {
    int new_block = alloc_data_block();
    if (new_block < 0) return -1;

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int ptrs_per_block = block_size / sizeof(DWORD);

    /* Aloca no ponteiro direto */
    if (logical_block < 2) {
        inode->dataPtr[logical_block] = new_block;
    } 
    /* Aloca no ponteiro indireto simples */
    else if (logical_block < 2 + ptrs_per_block) {
        if (inode->singleIndPtr == 0) {
            int ind_block = alloc_data_block();
            if (ind_block < 0) { free_data_block(new_block); return -1; }
            inode->singleIndPtr = ind_block;
            inode->blocksFileSize++;
        }
        unsigned char buf[block_size];
        if (read_block(inode->singleIndPtr, buf) != 0) return -1;
        DWORD *ptrs = (DWORD *)buf;
        ptrs[logical_block - 2] = new_block;
        if (write_block(inode->singleIndPtr, buf) != 0) return -1;
    } 
    /* Aloca no ponteiro indireto duplo implementado */
    else if (logical_block < 2 + ptrs_per_block + (ptrs_per_block * ptrs_per_block)) {
        if (inode->doubleIndPtr == 0) {
            int ind_block1 = alloc_data_block();
            if (ind_block1 < 0) { free_data_block(new_block); return -1; }
            inode->doubleIndPtr = ind_block1;
            inode->blocksFileSize++;
        }
        unsigned char buf1[block_size];
        if (read_block(inode->doubleIndPtr, buf1) != 0) return -1;
        DWORD *ptrs1 = (DWORD *)buf1;
        
        unsigned int double_idx = logical_block - 2 - ptrs_per_block;
        unsigned int idx1 = double_idx / ptrs_per_block;
        unsigned int idx2 = double_idx % ptrs_per_block;
        
        if (ptrs1[idx1] == 0) {
            int ind_block2 = alloc_data_block();
            if (ind_block2 < 0) { free_data_block(new_block); return -1; } 
            ptrs1[idx1] = ind_block2;
            if (write_block(inode->doubleIndPtr, buf1) != 0) return -1;
            inode->blocksFileSize++;
        }
        
        unsigned char buf2[block_size];
        if (read_block(ptrs1[idx1], buf2) != 0) return -1;
        DWORD *ptrs2 = (DWORD *)buf2;
        ptrs2[idx2] = new_block;
        if (write_block(ptrs1[idx1], buf2) != 0) return -1;
    }
    else {
        free_data_block(new_block);
        return -1;
    }
    inode->blocksFileSize++;
    return new_block;
}

/* Libera sistematicamente todos os blocos de dados associados a um i-node */
static void free_inode_blocks(struct sofs_inode *inode) {
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int ptrs_per_block = block_size / sizeof(DWORD);

    /* Libera blocos diretos */
    for (int i = 0; i < 2; i++) {
        if (inode->dataPtr[i] != 0) {
            free_data_block(inode->dataPtr[i]);
            inode->dataPtr[i] = 0;
        }
    }
    /* Libera blocos mapeados pela indireção simples */
    if (inode->singleIndPtr != 0) {
        unsigned char buf[block_size];
        if (read_block(inode->singleIndPtr, buf) == 0) {
            DWORD *ptrs = (DWORD *)buf;
            for (unsigned int i = 0; i < ptrs_per_block; i++) {
                if (ptrs[i] != 0) free_data_block(ptrs[i]);
            }
        }
        free_data_block(inode->singleIndPtr);
        inode->singleIndPtr = 0;
    }
    
    /* Libera os blocos mapeados pela indireção dupla */
    if (inode->doubleIndPtr != 0) {
        unsigned char buf1[block_size];
        if (read_block(inode->doubleIndPtr, buf1) == 0) {
            DWORD *ptrs1 = (DWORD *)buf1;
            for (unsigned int i = 0; i < ptrs_per_block; i++) {
                if (ptrs1[i] != 0) {
                    unsigned char buf2[block_size];
                    if (read_block(ptrs1[i], buf2) == 0) {
                        DWORD *ptrs2 = (DWORD *)buf2;
                        for (unsigned int j = 0; j < ptrs_per_block; j++) {
                            if (ptrs2[j] != 0) free_data_block(ptrs2[j]);
                        }
                    }
                    free_data_block(ptrs1[i]);
                }
            }
        }
        free_data_block(inode->doubleIndPtr);
        inode->doubleIndPtr = 0;
    }
    
    inode->blocksFileSize = 0;
    inode->bytesFileSize = 0;
}

/* Busca sequencialmente um arquivo pelo nome no Diretório Raiz (i-node 0) */
static int find_dir_entry(const char *name, struct sofs_record *out_rec, unsigned int *out_offset) {
    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0) return -1;

    unsigned int offset = 0;
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char buf[block_size];
    int current_phys_block = -1;

    while (offset < root_inode.bytesFileSize) {
        unsigned int logical_block = offset / block_size;
        unsigned int offset_in_block = offset % block_size;

        int phys_block = get_block_from_inode(&root_inode, logical_block);
        if (phys_block <= 0) break;

        /* Carrega o bloco para a memória apenas se o bloco físico mudar no iterador */
        if (current_phys_block != phys_block) {
            if (read_block(phys_block, buf) != 0) break;
            current_phys_block = phys_block;
        }

        struct sofs_record *rec = (struct sofs_record *)(buf + offset_in_block);
        if (rec->TypeVal != TYPEVAL_INVALIDO && strcmp(rec->name, name) == 0) {
            if (out_rec) *out_rec = *rec;
            if (out_offset) *out_offset = offset;
            return 0; /* Arquivo encontrado */
        }
        offset += sizeof(struct sofs_record);
    }
    return -1; /* Arquivo não encontrado */
}

/* Registra uma nova entrada no Diretório Raiz buscando slot vazio ou expandindo o tamanho */
static int add_dir_entry(const char *name, unsigned int inode_num, BYTE typeVal) {
    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0) return -1;

    unsigned int offset = 0;
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char buf[block_size];
    int current_phys_block = -1;
    unsigned int target_offset = 0xFFFFFFFF;
    int found_empty = 0;

    /* Pesquisa o primeiro slot inválido (disponível) dentro do tamanho atual do diretório */
    while (offset < root_inode.bytesFileSize) {
        unsigned int logical_block = offset / block_size;
        unsigned int offset_in_block = offset % block_size;

        int phys_block = get_block_from_inode(&root_inode, logical_block);
        if (phys_block <= 0) break;

        if (current_phys_block != phys_block) {
            if (read_block(phys_block, buf) != 0) return -1;
            current_phys_block = phys_block;
        }

        struct sofs_record *rec = (struct sofs_record *)(buf + offset_in_block);
        if (rec->TypeVal == TYPEVAL_INVALIDO) {
            target_offset = offset;
            found_empty = 1;
            break;
        }
        offset += sizeof(struct sofs_record);
    }

    /* Caso não encontre buracos, expande o final do arquivo de diretório */
    if (!found_empty) {
        target_offset = root_inode.bytesFileSize;
    }

    unsigned int logical_block = target_offset / block_size;
    unsigned int offset_in_block = target_offset % block_size;

    int phys_block = get_block_from_inode(&root_inode, logical_block);
    if (phys_block <= 0) {
        /* Se o offset exige um novo bloco que não existe, nós o alocamos */
        phys_block = allocate_block_for_inode(&root_inode, logical_block);
        if (phys_block < 0) return -1;
        memset(buf, 0, block_size);
    } else {
        if (read_block(phys_block, buf) != 0) return -1;
    }

    /* Modifica o bloco de dados mapeando a estrutura sofs_record */
    struct sofs_record *rec = (struct sofs_record *)(buf + offset_in_block);
    rec->TypeVal = typeVal;
    strncpy(rec->name, name, 50);
    rec->name[50] = '\0';
    rec->inodeNumber = inode_num;

    /* Persiste as modificações no disco */
    if (write_block(phys_block, buf) != 0) return -1;

    /* Atualiza os metadados de tamanho do diretório se este tiver expandido */
    if (target_offset + sizeof(struct sofs_record) > root_inode.bytesFileSize) {
        root_inode.bytesFileSize = target_offset + sizeof(struct sofs_record);
    }
    
    return write_inode(0, &root_inode);
}

/* Invalida logicamente uma entrada baseada no seu offset absoluto dentro do diretório */
static int remove_dir_entry(unsigned int offset) {
    struct sofs_inode root_inode;
    if (read_inode(0, &root_inode) != 0) return -1;

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned int logical_block = offset / block_size;
    unsigned int offset_in_block = offset % block_size;

    int phys_block = get_block_from_inode(&root_inode, logical_block);
    if (phys_block <= 0) return -1;

    unsigned char buf[block_size];
    if (read_block(phys_block, buf) != 0) return -1;

    struct sofs_record *rec = (struct sofs_record *)(buf + offset_in_block);
    rec->TypeVal = TYPEVAL_INVALIDO;

    return write_block(phys_block, buf);
}

/* -------------------------------------------------------------------------
 * Gerência do sistema de arquivos
 * ---------------------------------------------------------------------- */

int sofs_identify(char *name, int size)
{
    const char *id = "Grupo M";
    if (name == NULL || size <= 0)
        return -1;
    strncpy(name, id, size - 1);
    name[size - 1] = '\0';
    return 0;
}

int sofs_format(int partition, int sectors_per_block)
{
    unsigned int first_sector, num_sectors;
    unsigned int num_blocks;
    unsigned int inode_area_blocks;
    unsigned int bitmap_blocks_data;
    unsigned int bitmap_blocks_inode;
    unsigned char block_buf[sectors_per_block * SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (sectors_per_block <= 0)
        return -1;

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Inicializa a camada de blocos para poder escrever na partição */
    if (init_block_layer(first_sector, (unsigned int)sectors_per_block) != 0)
        return -1;

    num_blocks = num_sectors / (unsigned int)sectors_per_block;

    /* 10% dos blocos para i-nodes, arredondado para cima */
    inode_area_blocks = (num_blocks + 9) / 10;

    /* Um bloco por 8*(sectors_per_block*SECTOR_SIZE) bits necessários em cada bitmap */
    bitmap_blocks_data  = (num_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);
    bitmap_blocks_inode = (inode_area_blocks + 8 * sectors_per_block * SECTOR_SIZE - 1)
                          / (8 * sectors_per_block * SECTOR_SIZE);

    /* Constrói e grava o superbloco (bloco 0 da partição) */
    memset(block_buf, 0, sizeof(block_buf));
    sb = (struct sofs_superbloco *)block_buf;
    memcpy(sb->id, "SOFS", 4);
    sb->version              = 0x7E32;
    sb->superblockSize       = 1;
    sb->freeBlocksBitmapSize = (WORD)bitmap_blocks_data;
    sb->freeInodeBitmapSize  = (WORD)bitmap_blocks_inode;
    sb->inodeAreaSize        = (WORD)inode_area_blocks;
    sb->blockSize            = (WORD)sectors_per_block;
    sb->diskSize             = (DWORD)num_blocks;

    /* Checksum: complemento de um da soma dos 5 primeiros DWORDs */
    {
        DWORD *words = (DWORD *)block_buf;
        DWORD  sum   = words[0] + words[1] + words[2] + words[3] + words[4];
        sb->Checksum = ~sum;
    }

    if (write_block(0, block_buf) != 0)
        return -1;

    /* Inicializar com zeros as áreas de bitmap e de i-nodes */
    /* Inicializa fisicamente as áreas de bitmaps e i-nodes gravando zeros absolutos */
    unsigned int total_bitmap_blocks = bitmap_blocks_data + bitmap_blocks_inode;
    unsigned int total_blocks_to_zero = total_bitmap_blocks + inode_area_blocks;
    unsigned char zero_buf[sectors_per_block * SECTOR_SIZE];
    memset(zero_buf, 0, sizeof(zero_buf));

    for (unsigned int i = 1; i <= total_blocks_to_zero; i++) {
        if (write_block(i, zero_buf) != 0)
            return -1;
    }

    /* Montagem Virtual Temporária: Necessária para alocar e fixar o Inode 0 como Diretório Raiz. */
    sofs_mount(partition);
    int root_inode = alloc_inode(); 
    if (root_inode == 0) {
        struct sofs_inode ri;
        memset(&ri, 0, sizeof(ri));
        ri.RefCounter = 1;
        write_inode(0, &ri); /* Salva a inicialização do diretório no disco */
    }
    sofs_umount();

    return 0;
}

int sofs_mount(int partition)
{
    unsigned int first_sector, num_sectors;
    unsigned char sector_buf[SECTOR_SIZE];
    struct sofs_superbloco *sb;

    if (g_mounted)
        return -1;  /* partição já montada */

    if (read_partition_info(partition, &first_sector, &num_sectors) != 0)
        return -1;

    /* Lê o primeiro setor da partição para obter o superbloco */
    if (read_sector(first_sector, sector_buf) != 0)
        return -1;

    sb = (struct sofs_superbloco *)sector_buf;

    /* Valida a assinatura do sistema de arquivos */
    if (memcmp(sb->id, "SOFS", 4) != 0)
        return -1;

    /* Agora sabemos o tamanho do bloco: inicializa a camada de blocos */
    if (init_block_layer(first_sector, (unsigned int)sb->blockSize) != 0)
        return -1;

    /* Abre o subsistema de bitmap */
    g_superbloco_sector = first_sector;
    if (openBitmap2((int)g_superbloco_sector) != 0)
        return -1;

    /* Armazena em cache o superbloco */
    memcpy(&g_superbloco, sb, sizeof(g_superbloco));
    g_mounted = true;
    return 0;
}

int sofs_umount(void)
{
    if (!g_mounted)
        return -1;

    closeBitmap2();
    reset_block_layer();
    memset(&g_superbloco, 0, sizeof(g_superbloco));
    g_mounted = false;
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de arquivo
 * ---------------------------------------------------------------------- */

SOFS_FILE sofs_create(char *filename)
{
    /* Aloca um i-node (alloc_inode), adiciona um registro de diretório,
     * abre o arquivo e retorna um handle. Se o arquivo já existir,
     * trunca-o para zero bytes primeiro. */
    
     /* Validação rígida contra ponteiros nulos ou comprimentos erráticos */
    if (!g_mounted || !filename || strlen(filename) == 0 || strlen(filename) > SOFS_MAX_FILE_NAME_SIZE) 
        return -1;

    struct sofs_record rec;
    /* Se o arquivo já estiver contido no diretório raiz */
    if (find_dir_entry(filename, &rec, NULL) == 0) {
        
        /* Previne manipulação indevida sobre links na criação */
        if (rec.TypeVal == TYPEVAL_LINK) return -1; 
        
        /* Verifica se há espaço na tabela de arquivos abertos ANTES de truncar o arquivo existente */
        int handle_livre = -1;
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            if (!g_open_files[i].in_use) {
                handle_livre = i;
                break;
            }
        }
        if (handle_livre == -1) return -1; /* Limite de arquivos abertos atingido, aborta antes de destruir os dados */

        struct sofs_inode in;
        if (read_inode(rec.inodeNumber, &in) != 0) return -1;
        
        /* Truncamento: todos os blocos de dados vinculados são liberados para 0 bytes */
        free_inode_blocks(&in);
        write_inode(rec.inodeNumber, &in);

        /* Atribui e preenche um handle da Tabela de Arquivos Abertos */
        g_open_files[handle_livre].in_use = 1;
        g_open_files[handle_livre].inode_num = rec.inodeNumber;
        g_open_files[handle_livre].offset = 0;
        return handle_livre;
    }

    /* Novo Registro: Inicia a alocação do arquivo inexistente */
    int inode_num = alloc_inode();
    if (inode_num < 0) return -1;

    struct sofs_inode in;
    memset(&in, 0, sizeof(in));
    in.RefCounter = 1; /* O registro em diretório atua como a referência inicial */
    
    if (write_inode(inode_num, &in) != 0) {
        free_inode(inode_num);
        return -1;
    }

    /* Engaja o arquivo recém-criado na lista de entradas do Inode 0 */
    if (add_dir_entry(filename, inode_num, TYPEVAL_REGULAR) != 0) {
        free_inode(inode_num);
        return -1;
    }

    /* Atribui e preenche o correspondente handle em memória para uso contínuo */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!g_open_files[i].in_use) {
            g_open_files[i].in_use = 1;
            g_open_files[i].inode_num = inode_num;
            g_open_files[i].offset = 0;
            return i;
        }
    }

    return -1;
}

int sofs_delete(char *name)
{
    /* Localiza o registro de diretório de <name>, libera todos os blocos
     * de dados referenciados pelo i-node (free_data_block), libera o i-node
     * (free_inode) e invalida o registro de diretório. */

    if (!g_mounted || !name) return -1;

    unsigned int offset;
    struct sofs_record rec;
    
    /* Extrai o offset absoluto da entrada dentro da estrutura em disco do diretório */
    if (find_dir_entry(name, &rec, &offset) != 0) return -1;

    struct sofs_inode in;
    if (read_inode(rec.inodeNumber, &in) == 0) {
        /* Se houver outros hardlinks que exijam o conteúdo, ele apenas decrementa o contador */
        if (in.RefCounter > 1) {
            in.RefCounter--;
            write_inode(rec.inodeNumber, &in);
        } else {
            /* Se for o último vínculo nominal, erradica permanentemente os dados */
            free_inode_blocks(&in);
            free_inode(rec.inodeNumber);
        }
    }

    /* Neutraliza a entrada no arquivo do diretório, liberando o slot para reutilização futura */
    remove_dir_entry(offset);
    return 0;
}

SOFS_FILE sofs_open(char *name)
{
    /* Localiza o registro de diretório de <name>, verifica que o arquivo
     * existe, aloca uma entrada na tabela de arquivos abertos, inicializa o
     * ponteiro de posição em 0 e retorna o handle. */

    if (!g_mounted || !name) return -1;

    struct sofs_record rec;
    if (find_dir_entry(name, &rec, NULL) != 0) return -1;

    unsigned int inode_to_open = rec.inodeNumber;

    /* Lógica de Transição Transparente para Softlinks */
    /* Substituição do if por while com limite de iterações (max 10) para evitar ciclos infinitos */
    int num_indirections = 0;
    while (rec.TypeVal == TYPEVAL_LINK) {
        if (num_indirections >= 10) return -1; /* Aborta se detectado possível ciclo infinito */
        
        struct sofs_inode link_in;
        if (read_inode(rec.inodeNumber, &link_in) != 0) return -1;

        unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
        unsigned char buf[block_size];
        
        int phys_block = get_block_from_inode(&link_in, 0);
        if (phys_block <= 0 || read_block(phys_block, buf) != 0) return -1;

        char target_name[SOFS_MAX_FILE_NAME_SIZE + 1];
        strncpy(target_name, (char*)buf, SOFS_MAX_FILE_NAME_SIZE);
        target_name[SOFS_MAX_FILE_NAME_SIZE] = '\0';

        /* Resolve o i-node definitivo apontado pela string contida no data block do softlink */
        if (find_dir_entry(target_name, &rec, NULL) != 0) return -1;
        inode_to_open = rec.inodeNumber;
        
        num_indirections++;
    }

    /* Indexa o arquivo na tabela isolada */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!g_open_files[i].in_use) {
            g_open_files[i].in_use = 1;
            g_open_files[i].inode_num = inode_to_open;
            g_open_files[i].offset = 0;
            return i;
        }
    }
    return -1;
}

int sofs_close(SOFS_FILE handle)
{
    /* Valida <handle> e libera sua entrada na tabela de arquivos abertos. */

     /* Valida a coerência do manipulador recebido garantindo sua presença ativa na tabela */
    if (!g_mounted || handle < 0 || handle >= MAX_OPEN_FILES || !g_open_files[handle].in_use) 
        return -1;

    /* Abate o uso do slot na tabela isolada na RAM */
    g_open_files[handle].in_use = 0;
    return 0;
}

int sofs_read(SOFS_FILE handle, char *buffer, int size)
{
    /* Lê até <size> bytes do arquivo a partir da posição corrente;
     * avança o ponteiro de posição; retorna o número de bytes efetivamente lidos. */
    
    if (!g_mounted || handle < 0 || handle >= MAX_OPEN_FILES || !g_open_files[handle].in_use || !buffer || size < 0) 
        return -1;
    if (size == 0) return 0;

    struct open_file_entry *of = &g_open_files[handle];
    struct sofs_inode in;
    if (read_inode(of->inode_num, &in) != 0) return -1;

    if (of->offset >= in.bytesFileSize) return 0; /* Atingiu Fim de Arquivo (EOF) */

    /* Regula o limite matemático da leitura baseando-se no limite do bytesFileSize */
    unsigned int bytes_to_read = size;
    if (of->offset + bytes_to_read > in.bytesFileSize) {
        bytes_to_read = in.bytesFileSize - of->offset;
    }

    unsigned int bytes_read = 0;
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char buf[block_size];

    /* Algoritmo de Fracionamento Cíclico: lê iterativamente os bytes do disco convertendo offsets relativos */
    while (bytes_read < bytes_to_read) {
        unsigned int logical_block = of->offset / block_size;
        unsigned int offset_in_block = of->offset % block_size;
        unsigned int chunk = block_size - offset_in_block;
        
        if (chunk > bytes_to_read - bytes_read) 
            chunk = bytes_to_read - bytes_read;

        int phys_block = get_block_from_inode(&in, logical_block);
        if (phys_block <= 0 || read_block(phys_block, buf) != 0) return -1;

        memcpy(buffer + bytes_read, buf + offset_in_block, chunk);
        
        bytes_read += chunk;
        of->offset += chunk; /* Avança o apontador estrito do usuário na memória */
    }

    return bytes_read;
}

int sofs_write(SOFS_FILE handle, char *buffer, int size)
{
    /* Grava <size> bytes no arquivo a partir da posição corrente,
     * alocando novos blocos de dados conforme necessário (alloc_data_block);
     * avança o ponteiro de posição; retorna o número de bytes gravados. */
    
    if (!g_mounted || handle < 0 || handle >= MAX_OPEN_FILES || !g_open_files[handle].in_use || !buffer || size < 0) 
        return -1;
    if (size == 0) return 0;

    struct open_file_entry *of = &g_open_files[handle];
    struct sofs_inode in;
    if (read_inode(of->inode_num, &in) != 0) return -1;

    unsigned int bytes_written = 0;
    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char buf[block_size];

    while (bytes_written < (unsigned int)size) {
        unsigned int logical_block = of->offset / block_size;
        unsigned int offset_in_block = of->offset % block_size;
        unsigned int chunk = block_size - offset_in_block;
        
        if (chunk > (unsigned int)size - bytes_written) 
            chunk = (unsigned int)size - bytes_written;

        int phys_block = get_block_from_inode(&in, logical_block);
        
        /* Delegação autônoma de blocos dinâmicos quando a escrita atravessa a barreira do EOF */
        if (phys_block <= 0) {
            phys_block = allocate_block_for_inode(&in, logical_block);
            if (phys_block < 0) break; /* Rompeu a capacidade global livre de disco */
            memset(buf, 0, block_size);
        } else {
            if (read_block(phys_block, buf) != 0) break;
        }

        memcpy(buf + offset_in_block, buffer + bytes_written, chunk);
        if (write_block(phys_block, buf) != 0) break;

        bytes_written += chunk;
        of->offset += chunk;
    }

    /* Consolida expansão volumétrica caso o ponteiro desloque para o além-marcado */
    if (of->offset > in.bytesFileSize) {
        in.bytesFileSize = of->offset;
    }
    
    /* Relê apenas os campos vulneráveis do i-node (como RefCounter) do disco para evitar sobrescrita por concorrência antes do salvamento final */
    struct sofs_inode in_concorrente;
    if (read_inode(of->inode_num, &in_concorrente) == 0) {
        in.RefCounter = in_concorrente.RefCounter;
    }
    
    write_inode(of->inode_num, &in);

    if (bytes_written == 0 && size > 0) return -1;
    return bytes_written;
}

/* -------------------------------------------------------------------------
 * Operações de diretório
 * ---------------------------------------------------------------------- */

int sofs_opendir(void)
{
    /* Verifica que uma partição está montada, posiciona o ponteiro de
     * entradas no primeiro registro válido do diretório raiz e retorna 0. */
    
    if (!g_mounted) return -1;
    g_dir_offset = 0; /* Configuração de reset estrutural do ponteiro em memória */
    return 0;
}

int sofs_readdir(SOFS_DIRENT *dentry)
{
    /* Lê o próximo registro válido do diretório em *dentry e avança o
     * ponteiro de entradas. Retorna valor diferente de zero ao fim do diretório. */
    
    if (!g_mounted || !dentry) return -1;

    struct sofs_inode root_in;
    if (read_inode(0, &root_in) != 0) return -1;

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char buf[block_size];
    int current_phys_block = -1;

    /* Varredura progressiva sobre a trilha contínua do root_inode ignorando os slots em vazio */
    while (g_dir_offset < root_in.bytesFileSize) {
        unsigned int logical_block = g_dir_offset / block_size;
        unsigned int offset_in_block = g_dir_offset % block_size;

        int phys_block = get_block_from_inode(&root_in, logical_block);
        if (phys_block <= 0) return -1;

        if (current_phys_block != phys_block) {
            if (read_block(phys_block, buf) != 0) return -1;
            current_phys_block = phys_block;
        }

        struct sofs_record *rec = (struct sofs_record *)(buf + offset_in_block);
        g_dir_offset += sizeof(struct sofs_record);

        /* Extração nominal caso registro se prove intacto */
        if (rec->TypeVal != TYPEVAL_INVALIDO) {
            strncpy(dentry->name, rec->name, SOFS_MAX_FILE_NAME_SIZE);
            dentry->name[SOFS_MAX_FILE_NAME_SIZE] = '\0';
            dentry->fileType = rec->TypeVal;

            struct sofs_inode target_in;
            if (read_inode(rec->inodeNumber, &target_in) == 0) {
                dentry->fileSize = target_in.bytesFileSize;
            } else {
                dentry->fileSize = 0;
            }
            return 0;
        }
    }
    return -1; /* O fim do volume de bytes de escopo indica ausência de diretivas restantes */
}

int sofs_closedir(void)
{
    /* Reinicia o ponteiro de entradas do diretório e retorna 0. */

    if (!g_mounted) return -1;
    g_dir_offset = 0; /* Expurga o estado persistido */
    return 0;
}

/* -------------------------------------------------------------------------
 * Operações de link
 * ---------------------------------------------------------------------- */

int sofs_sln(char *linkname, char *filename)
{
    /* Cria um softlink chamado <linkname> cujo único bloco de dados
     * contém o string <filename>. */
    
    if (!g_mounted || !linkname || !filename) return -1;

    struct sofs_record dummy;
    if (find_dir_entry(linkname, &dummy, NULL) == 0) return -1;

    int inode_num = alloc_inode();
    if (inode_num < 0) return -1;

    struct sofs_inode in;
    memset(&in, 0, sizeof(in));
    in.RefCounter = 1;

    /* O softlink aloja o string nominal do alvo nativamente em seu primeiro bloco de dados direto */
    int phys_block = allocate_block_for_inode(&in, 0);
    if (phys_block < 0) { free_inode(inode_num); return -1; }

    unsigned int block_size = g_superbloco.blockSize * SECTOR_SIZE;
    unsigned char buf[block_size];
    memset(buf, 0, block_size);
    strncpy((char*)buf, filename, block_size - 1);
    write_block(phys_block, buf);

    in.bytesFileSize = strlen(filename);
    write_inode(inode_num, &in);

    if (add_dir_entry(linkname, inode_num, TYPEVAL_LINK) != 0) {
        free_inode_blocks(&in);
        free_inode(inode_num);
        return -1;
    }
    return 0;
}

int sofs_hln(char *linkname, char *filename)
{
    /* Cria um hardlink chamado <linkname> apontando para o mesmo
     * i-node que <filename>; incrementa o campo RefCounter do i-node. */
    
    if (!g_mounted || !linkname || !filename) return -1;

    struct sofs_record target_rec;
    if (find_dir_entry(filename, &target_rec, NULL) != 0) return -1; 
    
    /* O sistema permite criar hardlinks apontando para softlinks validamente, seguindo o padrão UNIX */
    /* if (target_rec.TypeVal == TYPEVAL_LINK) return -1; */ 

    struct sofs_record dummy;
    if (find_dir_entry(linkname, &dummy, NULL) == 0) return -1; 

    struct sofs_inode target_in;
    if (read_inode(target_rec.inodeNumber, &target_in) != 0) return -1;

    /* O espelhamento exato de hardlink não aloca memória nova; ele apenas amplifica contadores */
    target_in.RefCounter++;
    write_inode(target_rec.inodeNumber, &target_in);

    if (add_dir_entry(linkname, target_rec.inodeNumber, target_rec.TypeVal) != 0) {
        target_in.RefCounter--;
        write_inode(target_rec.inodeNumber, &target_in);
        return -1;
    }
    return 0;
}
