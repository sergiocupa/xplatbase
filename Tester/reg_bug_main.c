/* Wrapper para rodar a suite de bugs (thread_pool_bug_test.c) standalone.
 * Encaminha o argv real (necessario para os testes que relancham processo-filho
 * via "--bug-child <mode>"); sem args, injeta "--bugs". */
extern int thread_pool_bug_test_dispatch(int argc, char** argv);

int main(int argc, char** argv)
{
    if (argc <= 1) {
        char* a[] = { argv[0], "--bugs" };
        return thread_pool_bug_test_dispatch(2, a);
    }
    return thread_pool_bug_test_dispatch(argc, argv);
}
