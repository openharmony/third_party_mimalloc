#include <regex.h>
#include <alloca.h>
#include <string.h>
#include <stdio.h>

#include "mimalloc.h"
#include "mimalloc-types.h"
#include "testhelper.h"
#include "libxml/parser.h"

#define NORMAL_ALLOCATIONS_COUNT 11
#define LARGE_ALLOCATIONS_COUNT 8
#define XML_BUFFER_SIZE 16384
#define THREAD_ID_BUFFER_SIZE 65

xmlChar xml_buffer[XML_BUFFER_SIZE];

size_t normal_sizes[NORMAL_ALLOCATIONS_COUNT] = {8, 16, 32, 48, 64, 80, 96, 112,
                                                 128, 160, 192};
size_t large_sizes[LARGE_ALLOCATIONS_COUNT] = {163840, 196608, 229376, 262144, 327680, 393216, 458752, 524288};

typedef struct {
    double total;
    double current;
    double freed;
    unsigned long count;
} allocations_data_t;

static const xmlChar *get_attribute(const char *attr_name, xmlNodePtr node) {
  for (xmlAttrPtr curr_attr = node->properties; curr_attr != NULL; curr_attr = curr_attr->next) {
    if (xmlStrEqual(curr_attr->name, (const xmlChar *) attr_name)) {
      return curr_attr->children->content;
    }
  }
  return NULL;
}

static xmlNodePtr
find_child_node_with_attr(const char *name, const char *attr_name, const char *attr_value, xmlNodePtr parent) {
  if (parent == NULL) {
    return NULL;
  }
  for (xmlNodePtr curr_node = parent->children; curr_node != NULL; curr_node = curr_node->next) {
    if (curr_node->type == XML_ELEMENT_NODE && xmlStrEqual(curr_node->name, (xmlChar *) name)) {
      if (attr_name == NULL) {
        return curr_node;
      }
      if (xmlStrEqual(get_attribute(attr_name, curr_node), (const xmlChar *) attr_value)) {
        return curr_node;
      }
    }
  }
  return NULL;
}

static xmlNodePtr find_child_node(const char *name, xmlNodePtr parent) {
  return find_child_node_with_attr(name, NULL, NULL, parent);
}

static const char *get_node_text(xmlNodePtr node_ptr) {

  return node_ptr == NULL ? NULL : (const char *) node_ptr->children->content;
}

static bool validate_format(const char *s, const regex_t *regex, bool optional) {
  if (s == NULL) {
    return optional;
  }
  return regexec(regex, s, 0, NULL, 0) == 0;
}

unsigned long get_count(const char *count_s) {
  if (count_s == NULL) {
    return 0;
  }
  const char last = count_s[strlen(count_s) - 1];
  double fraction = strtod(count_s, NULL);
  switch (last) {
  case 'K':
    return (unsigned long) (fraction * 1000);
  case 'M':
    return (unsigned long) (fraction * 1000000);
  case 'G':
    return (unsigned long) (fraction * 1000000000);
  default:
    return strtoul(count_s, NULL, 10);
  }
}

static bool
populate_allocations(allocations_data_t *allocations_data, const char *type, xmlDocPtr doc, const regex_t *alloc_regex,
    const regex_t *count_regex) {
  char current_thread_id[THREAD_ID_BUFFER_SIZE];
  snprintf(current_thread_id, THREAD_ID_BUFFER_SIZE, "%zu", mi_heap_get_default()->thread_id);
  xmlNodePtr heap_root = find_child_node_with_attr("heap", "thread_id", current_thread_id, xmlDocGetRootElement(doc));
  xmlNodePtr allocations_by_type = find_child_node(type,
      find_child_node("allocations", heap_root));
  const char *total = get_node_text(find_child_node("total", allocations_by_type));
  const char *current = get_node_text(find_child_node("current", allocations_by_type));
  const char *freed = get_node_text(find_child_node("freed", allocations_by_type));
  const char *count = get_node_text(find_child_node("count", allocations_by_type));
  bool valid = validate_format(total, alloc_regex, false);
  valid &= validate_format(current, alloc_regex, false);
  valid &= validate_format(freed, alloc_regex, false);
  valid &= validate_format(count, count_regex, true);
  if (!valid) {
    return false;
  }
  *allocations_data = (allocations_data_t) {
      .total = strtod(total, NULL),
      .current = strtod(current, NULL),
      .freed = strtod(freed, NULL),
      .count = get_count(count)
  };
  return true;
}

//NOTICE: this allocates memory
static xmlDocPtr get_doc_ptr() {
  FILE *output_fp = fmemopen(xml_buffer, XML_BUFFER_SIZE, "w");
  if (output_fp == NULL) {
    return NULL;
  }
  mi_malloc_info(0, output_fp);
  fclose(output_fp);

  return xmlParseDoc(xml_buffer);
}

static bool test_allocations(const size_t *sizes, size_t allocation_count, const char *allocation_type,
    const regex_t *alloc_regex, const regex_t *count_regex) {
  xmlDocPtr doc_before_malloc = get_doc_ptr();
  if (doc_before_malloc == NULL) {
    return false;
  }
  void **ptrs = alloca(sizeof(void *) * allocation_count);
  for (size_t i = 0; i < allocation_count; i++) {
    ptrs[i] = mi_malloc(sizes[i]);
  }
  xmlDocPtr doc_after_malloc = get_doc_ptr();
  for (size_t i = 0; i < allocation_count; i++) {
    mi_free(ptrs[i]);
  }
  if (doc_after_malloc == NULL) {
    xmlFreeDoc(doc_before_malloc);
    return false;
  }
  xmlDocPtr doc_after_free = get_doc_ptr();
  if (doc_after_free == NULL) {
    xmlFreeDoc(doc_before_malloc);
    xmlFreeDoc(doc_after_malloc);
    return false;
  }

  allocations_data_t allocations_before_malloc;
  allocations_data_t allocations_after_malloc;
  allocations_data_t allocations_after_free;
  bool result = populate_allocations(&allocations_before_malloc, allocation_type, doc_before_malloc, alloc_regex,
      count_regex);
  result &= populate_allocations(&allocations_after_malloc, allocation_type, doc_after_malloc, alloc_regex,
      count_regex);
  result &= populate_allocations(&allocations_after_free, allocation_type, doc_after_free, alloc_regex, count_regex);
  if (!result) {
    return false;
  }
  result &= allocations_after_malloc.count >= allocations_before_malloc.count + allocation_count;
  result &= allocations_after_malloc.total >= allocations_before_malloc.total;

  result &= allocations_after_free.freed >= allocations_after_malloc.freed;

  xmlFreeDoc(doc_before_malloc);
  xmlFreeDoc(doc_after_malloc);
  xmlFreeDoc(doc_after_free);
  return result;
}

int main(void) {
  regex_t alloc_regex;
  regex_t count_regex;
  //NOTICE: this allocates memory
  CHECK_BODY("prepare-test-suite", {
    if (regcomp(&alloc_regex, "^[[:digit:]]+((\\.[[:digit:]]+)*[[:space:]][KMG]i)*$",
        REG_EXTENDED) != 0) {
      result = false;
      return print_test_summary();
    }
    if (regcomp(&count_regex, "^[[:digit:]]+((\\.[[:digit:]]+)*[[:space:]][KMG])*$",
        REG_EXTENDED) != 0) {
      result = false;
      regfree(&alloc_regex);
      return print_test_summary();
    }
  });

  CHECK_BODY("mi_malloc_info-parseable-xml", {
    xmlDocPtr doc_ptr = get_doc_ptr();
    if (doc_ptr == NULL) {
      result = false;
    } else {
      xmlFreeDoc(doc_ptr);
    }
  });
  CHECK_BODY("mi_malloc_info-normal-allocations-show-up", {
    result = test_allocations(normal_sizes, NORMAL_ALLOCATIONS_COUNT, "normal", &alloc_regex, &count_regex);
  });
  CHECK_BODY("mi_malloc_info-large-allocations-show-up", {
    result = test_allocations(large_sizes, LARGE_ALLOCATIONS_COUNT, "large", &alloc_regex, &count_regex);
  });

  xmlCleanupParser();
  regfree(&alloc_regex);
  regfree(&count_regex);

  return print_test_summary();
}