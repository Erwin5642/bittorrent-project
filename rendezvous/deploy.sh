#!/usr/bin/env bash
# Publica o servidor de rendezvous na AWS (EC2 + IP elástico, imagem no ECR).
#
# uso: rendezvous/deploy.sh <comando>
#   up        cria o repositório ECR, envia a imagem e cria/atualiza a stack
#   update    envia uma imagem nova e reinicia o container (sem recriar a instância)
#   status    mostra o endereço e testa o servidor com PING
#   logs      últimas linhas do log (CloudWatch)
#   destroy   remove a stack (use --purge para apagar também o repositório ECR)
#
# Precisa de: aws cli v2 (já autenticada), docker, bash. Variáveis (opcionais):
#   AWS_REGION (padrão: a do `aws configure`, senão us-east-1), STACK (bt-rendezvous),
#   PORT (7000), TTL (60), ALLOWED_CIDR (0.0.0.0/0), INSTANCE_TYPE (t3.micro)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STACK="${STACK:-bt-rendezvous}"
REPO="${REPO:-bt-rendezvous}"
export AWS_REGION="${AWS_REGION:-$(aws configure get region 2>/dev/null || true)}"
export AWS_REGION="${AWS_REGION:-us-east-1}"
export AWS_DEFAULT_REGION="$AWS_REGION"

need() { command -v "$1" >/dev/null || { echo "erro: falta o comando $1" >&2; exit 1; }; }
out() { aws cloudformation describe-stacks --stack-name "$STACK" --query "Stacks[0].Outputs[?OutputKey=='$1'].OutputValue" --output text; }

account() { aws sts get-caller-identity --query Account --output text; }
registry() { echo "$(account).dkr.ecr.$AWS_REGION.amazonaws.com"; }
image() { echo "$(registry)/$REPO:latest"; }

push_image() {
    need docker
    aws ecr describe-repositories --repository-names "$REPO" >/dev/null 2>&1 ||
        aws ecr create-repository --repository-name "$REPO" --image-scanning-configuration scanOnPush=true >/dev/null
    aws ecr get-login-password | docker login --username AWS --password-stdin "$(registry)"
    # A instância é x86_64 (t3); força a arquitetura mesmo se você estiver num Mac M1.
    docker build --platform linux/amd64 -t "$(image)" "$HERE"
    docker push "$(image)"
}

case "${1:-}" in
up)
    need aws
    push_image
    aws cloudformation deploy --stack-name "$STACK" --template-file "$HERE/aws-template.yaml" \
        --capabilities CAPABILITY_IAM --no-fail-on-empty-changeset \
        --parameter-overrides "ImageUri=$(image)" "Port=${PORT:-7000}" "TtlSeconds=${TTL:-60}" \
        "AllowedCidr=${ALLOWED_CIDR:-0.0.0.0/0}" "InstanceType=${INSTANCE_TYPE:-t3.micro}"
    echo
    echo "Servidor de rendezvous: $(out Address)"
    echo "Cole esse endereço na aba Configuração do app."
    ;;
update)
    need aws
    push_image
    id="$(out InstanceId)"
    cmd="$(aws ssm send-command --instance-ids "$id" --document-name AWS-RunShellScript \
        --parameters "commands=[\"/usr/local/bin/rdv-update.sh $(image)\"]" \
        --query Command.CommandId --output text)"
    aws ssm wait command-executed --command-id "$cmd" --instance-id "$id"
    echo "container reiniciado com a imagem nova"
    ;;
status)
    need aws
    addr="$(out Address)"
    echo "endereço: $addr"
    bash "$HERE/rdv.sh" "$addr" ping
    ;;
logs)
    need aws
    aws logs tail "$(out LogGroup)" --since "${SINCE:-30m}"
    ;;
destroy)
    need aws
    aws cloudformation delete-stack --stack-name "$STACK"
    aws cloudformation wait stack-delete-complete --stack-name "$STACK"
    if [[ "${2:-}" == "--purge" ]]; then
        aws ecr delete-repository --repository-name "$REPO" --force >/dev/null
    fi
    echo "removido"
    ;;
*)
    sed -n '2,15p' "$0" >&2
    exit 2
    ;;
esac
