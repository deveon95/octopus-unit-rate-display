set(srcs "main.c")

idf_component_register(SRCS ${srcs}
	INCLUDE_DIRS "."
	EMBED_TXTFILES octopus_energy_root_cert.pem)
