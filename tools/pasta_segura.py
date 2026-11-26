# Validação dos caminhos da linha de comando dos scripts de tema (SonarCloud pythonsecurity:S8705/S8707):
# a origem precisa ser uma pasta existente e o destino fica confinado à pasta do projeto.
import os, sys


def pasta_origem(caminho):
    p = os.path.realpath(caminho)
    if not os.path.isdir(p):
        sys.exit('pacote não encontrado: ' + p)
    return p


def pasta_destino(caminho, raiz):
    p = os.path.realpath(caminho)
    try:
        dentro = os.path.commonpath((p, raiz)) == raiz
    except ValueError:
        dentro = False
    if not dentro:
        sys.exit('destino fora do projeto: ' + p)
    return p
