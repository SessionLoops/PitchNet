#if JucePlugin_Enable_ARA && JucePlugin_Build_AAX
 #define INITACFIDS
 #include <NonPublic/ARA/ARAAAX_UIDs.h>
 #undef INITACFIDS
 #undef DEFINE_ACFUID
 #define DEFINE_ACFUID(type, name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) ACFEXTERN_C const type name
 #include <NonPublic/ARA/AAX_VARABinding.cpp>
#endif
