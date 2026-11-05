/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace
{
class AIFunctionContracts : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  ObJsonObject *object(const char *text)
  {
    ObString input(text);
    ObJsonObject *result = nullptr;
    EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, result));
    return result;
  }
};

TEST_F(AIFunctionContracts, RepeatedPlaceholdersGrowWithOccurrences)
{
  const std::string large(4096, 'x');
  ObJsonObject *prompt = object("{\"template\":\"{0}/{0}/{0}\",\"args\":[\"\"]}");
  ASSERT_NE(nullptr, prompt);
  ObJsonArray *args = static_cast<ObJsonArray *>(prompt->get_value("args"));
  ASSERT_EQ(OB_SUCCESS, args->remove(0));
  ObString input(large.size(), large.data());
  ObJsonString *value = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator_, input, value));
  ASSERT_EQ(OB_SUCCESS, args->append(value));
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  EXPECT_EQ(large + "/" + large + "/" + large, std::string(output.ptr(), output.length()));
}

TEST_F(AIFunctionContracts, StructuredAndNestedArguments)
{
  ObJsonObject *prompt = object(
      "{\"template\":\"{0}|{1}|{2}\",\"args\":[[1,true],{\"x\":2},"
      "{\"template\":\"nested {0}\",\"args\":[\"你好\"]}]}");
  ObString output;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, prompt, output));
  const std::string text(output.ptr(), output.length());
  EXPECT_NE(std::string::npos, text.find("[1, true]"));
  EXPECT_NE(std::string::npos, text.find("\"x\": 2"));
  EXPECT_NE(std::string::npos, text.find("nested 你好"));
}

TEST_F(AIFunctionContracts, InvalidAndOverflowingPlaceholderIndices)
{
  const char *inputs[] = {
    "{\"template\":\"{1}\",\"args\":[\"x\"]}",
    "{\"template\":\"{9999999999999999999999999999999}\",\"args\":[\"x\"]}",
    "{\"template\":\"{0\",\"args\":[\"x\"]}"
  };
  for (const char *text : inputs) {
    ObString output;
    EXPECT_EQ(OB_INVALID_ARGUMENT,
        ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator_, object(text), output));
  }
}

TEST_F(AIFunctionContracts, EmbeddingResponseRestoresIndexOrder)
{
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *output = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator_, object(
      "{\"data\":[{\"index\":1,\"embedding\":[2.0]},{\"index\":0,\"embedding\":[1.0]}]}"), output));
  ASSERT_NE(nullptr, output);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, output->json_type());
  ObJsonArray *array = static_cast<ObJsonArray *>(output);
  EXPECT_DOUBLE_EQ(1.0, static_cast<ObJsonArray *>(array->get_value(0))->get_value(0)->get_double());
  EXPECT_DOUBLE_EQ(2.0, static_cast<ObJsonArray *>(array->get_value(1))->get_value(0)->get_double());
}

TEST_F(AIFunctionContracts, RejectsMalformedEmbeddingEnvelopes)
{
  const char *inputs[] = {
    "{}", "{\"data\":null}", "{\"data\":{}}", "{\"data\":[]}",
    "{\"data\":[1]}", "{\"data\":[{\"embedding\":[1]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":null}]}",
    "{\"data\":[{\"index\":-1,\"embedding\":[1]}]}",
    "{\"data\":[{\"index\":1,\"embedding\":[1]}]}",
    "{\"data\":[{\"index\":false,\"embedding\":[1]}]}",
    "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"index\":0,\"embedding\":[2]}]}"
  };
  ObOpenAIUtils::ObOpenAIEmbed provider;
  for (const char *text : inputs) {
    ObIJsonBase *output = nullptr;
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator_, object(text), output));
    EXPECT_EQ(nullptr, output);
  }
}

TEST_F(AIFunctionContracts, RejectsEmptyOrNonStringCompletion)
{
  ObOpenAIUtils::ObOpenAIComplete provider;
  const char *inputs[] = {
    "{}", "{\"choices\":[]}",
    "{\"choices\":[{\"message\":{\"content\":\"\"}}]}",
    "{\"choices\":[{\"message\":{\"content\":null}}]}",
    "{\"choices\":[{\"message\":{\"content\":123}}]}"
  };
  for (const char *text : inputs) {
    ObIJsonBase *output = nullptr;
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator_, object(text), output));
  }
}

TEST_F(AIFunctionContracts, OllamaUsesNativeNonStreamingBodyAndNoApiKey)
{
  ObOllamaUtils::ObOllamaComplete provider;
  ObArray<ObString> headers;
  ObString key;
  ASSERT_EQ(OB_SUCCESS, provider.get_header(allocator_, key, headers));
  ASSERT_EQ(1, headers.count());
  ObString model("arbitrary-model");
  ObString system("instruction");
  ObString input("test input");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator_, model, system, input,
      object("{\"temperature\":0.25,\"max_tokens\":16}"), body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  EXPECT_EQ(nullptr, body->get_value("messages"));
  ObJsonNode *options = body->get_value("options");
  ASSERT_NE(nullptr, options);
  ASSERT_EQ(ObJsonNodeType::J_OBJECT, options->json_type());
  EXPECT_EQ(16, static_cast<ObJsonObject *>(options)->get_value("num_predict")->get_int());
}

TEST_F(AIFunctionContracts, RejectsScalarConfig)
{
  for (const char *text : {"[]", "null", "1", "true", "\"text\""}) {
    ObString input(text);
    ObJsonObject *output = nullptr;
    EXPECT_EQ(OB_INVALID_ARGUMENT,
        ObAIFuncJsonUtils::get_json_object_form_str(allocator_, input, output));
  }
}
} // namespace
