#include "config_yaml.h"

#include <stdio.h>
#include <string.h>

static int passed;
static int failed;

#define CHECK(cond, name) do { \
	if (cond) { \
		passed++; \
	} else { \
		failed++; \
		fprintf(stderr, "FAIL: %s\n", name); \
	} \
} while (0)

static Node *parse(const char *yaml)
{
	return config_yaml_parse_memory((const unsigned char *)yaml,
		strlen(yaml));
}

static void test_mapping(void)
{
	Node *root = parse("server:\n  control_enable: true\n");
	CHECK(root && root->type == NODE_MAPPING, "mapping root");
	CHECK(root && root->child && !strcmp(root->child->key, "server"),
		"mapping key");
	CHECK(root && root->child && root->child->type == NODE_MAPPING,
		"nested mapping");
	config_yaml_free(root);
}

static void test_sequence(void)
{
	Node *root = parse("items:\n  - one\n  - two\n  - nested:\n      - three\n");
	Node *items = root ? root->child : NULL;
	CHECK(items && items->type == NODE_SEQUENCE, "sequence node");
	CHECK(items && items->child && !strcmp(items->child->value, "one"),
		"sequence first value");
	CHECK(items && items->child && items->child->next
		&& !strcmp(items->child->next->value, "two"),
		"sequence second value");
	CHECK(items && items->child && items->child->next
		&& items->child->next->next
		&& items->child->next->next->type == NODE_MAPPING,
		"nested sequence mapping");
	config_yaml_free(root);
}

static void test_multiple_documents(void)
{
	Node *root = parse("---\na: one\n---\nb: two\n");
	CHECK(root && root->next, "multiple document roots");
	CHECK(root && root->child && !strcmp(root->child->key, "a"),
		"first document");
	CHECK(root && root->next && root->next->child
		&& !strcmp(root->next->child->key, "b"), "second document");
	config_yaml_free(root);
}

static void test_invalid_inputs(void)
{
	static const unsigned char timeout_regression[] = {'\n', '\n', '}', '\n'};

	CHECK(config_yaml_parse_memory(NULL, 0) == NULL, "null input");
	CHECK(config_yaml_parse_memory((const unsigned char *)"", 0) == NULL,
		"empty input");
	CHECK(config_yaml_parse_memory(timeout_regression,
		sizeof(timeout_regression)) == NULL, "timeout regression");
	CHECK(parse("key: [unterminated\n") == NULL, "malformed sequence");
}

int main(void)
{
	test_mapping();
	test_sequence();
	test_multiple_documents();
	test_invalid_inputs();

	printf("YAML parser: %d passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
